// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "image_resources.h"
#include <memory>
#include <map>
#include <zen/utf.h>
#include <zen/globals.h>
#include <zen/perf.h>
#include <zen/thread.h>
#include <zen/file_io.h>
#include <zen/file_traverser.h>
#include <wx/zipstrm.h>
#include <wx/mstream.h>
#include <wx/image.h>
#include <xBRZ/src/xbrz.h>
#include <xBRZ/src/xbrz_tools.h>
#include "image_tools.h"
#include "image_holder.h"
#include "dc.h"

using namespace zen;


namespace
{
ImageHolder dpiScale(int width, int height, int dpiWidth, int dpiHeight, const unsigned char* imageRgb, const unsigned char* imageAlpha, int hqScale)
{
    assert(imageRgb && imageAlpha); //see convertToVanillaImage()
    if (width <= 0 || height <= 0 || dpiWidth <= 0 || dpiHeight <= 0)
        return ImageHolder(0, 0, true /*withAlpha*/);

    const int hqWidth  = width  * hqScale;
    const int hqHeight = height * hqScale;

    //get rid of allocation and buffer std::vector<> at thread-level? => no discernable perf improvement
    std::vector<uint32_t> buf(hqWidth * hqHeight + std::max(width * height, dpiWidth * dpiHeight));
    uint32_t* const argbSrc = &buf[0] + hqWidth * hqHeight;
    uint32_t* const xbrTrg  = &buf[0];
    uint32_t* const dpiTrg  = argbSrc;

    //convert RGB (RGB byte order) to ARGB (BGRA byte order)
    {
        const unsigned char* rgb = imageRgb;
        const unsigned char* rgbEnd = rgb + 3 * width * height;
        const unsigned char* alpha  = imageAlpha;
        uint32_t* out = argbSrc;

        for (; rgb < rgbEnd; rgb += 3)
            *out++ = xbrz::makePixel(*alpha++, rgb[0], rgb[1], rgb[2]);
    }
    //-----------------------------------------------------
    xbrz::scale(hqScale,       //size_t factor, //valid range: 2 - SCALE_FACTOR_MAX
                argbSrc,       //const uint32_t* src,
                xbrTrg,        //uint32_t* trg,
                width, height, //int srcWidth, int srcHeight,
                xbrz::ColorFormat::ARGB_UNBUFFERED); //ColorFormat colFmt,
    //test: total xBRZ scaling time with ARGB: 300ms, ARGB_UNBUFFERED: 50ms
    //-----------------------------------------------------
    xbrz::bilinearScale(xbrTrg,               //const uint32_t* src,
                        hqWidth, hqHeight,    //int srcWidth, int srcHeight,
                        dpiTrg,               //uint32_t* trg,
                        dpiWidth, dpiHeight); //int trgWidth, int trgHeight
    //-----------------------------------------------------
    //convert BGRA to RGB + alpha
    ImageHolder trgImg(dpiWidth, dpiHeight, true /*withAlpha*/);
    {
        unsigned char* rgb   = trgImg.getRgb();
        unsigned char* alpha = trgImg.getAlpha();

        std::for_each(dpiTrg, dpiTrg + dpiWidth * dpiHeight, [&](uint32_t col)
        {
            *alpha++ = xbrz::getAlpha(col);
            *rgb++   = xbrz::getRed  (col);
            *rgb++   = xbrz::getGreen(col);
            *rgb++   = xbrz::getBlue (col);
        });
    }
    return trgImg;
}


auto getScalerTask(const wxString& name, const wxImage& img, int hqScale, Protected<std::vector<std::pair<std::wstring, ImageHolder>>>& result)
{
    return [name = copyStringTo<std::wstring>(name), //don't trust wxString to be thread-safe like an int
                 width  = img.GetWidth(),
                 height = img.GetHeight(),
                 dpiWidth  = fastFromDIP(img.GetWidth()),
                 dpiHeight = fastFromDIP(img.GetHeight()), //don't call (wxWidgets function!) fastFromDIP() from worker thread
                 rgb   = img.GetData(),
                 alpha = img.GetAlpha(),
                 hqScale, &result]
    {
        ImageHolder ih = dpiScale(width,    height,
                                  dpiWidth, dpiHeight,
                                  rgb, alpha, hqScale);
        result.access([&](std::vector<std::pair<std::wstring, ImageHolder>>& r) { r.emplace_back(name, std::move(ih)); });
    };
}


class DpiParallelScaler
{
public:
    DpiParallelScaler(int hqScale) : hqScale_(hqScale) { assert(hqScale > 1); }

    ~DpiParallelScaler() { threadGroup_ = {}; } //DpiParallelScaler must out-live threadGroup!!!

    void add(const wxString& name, const wxImage& img)
    {
        imgKeeper_.push_back(img); //retain (ref-counted) wxImage so that the rgb/alpha pointers remain valid after passed to threads
        threadGroup_->run(getScalerTask(name, img, hqScale_, result_));
    }

    std::map<wxString, wxBitmap> waitAndGetResult()
    {
        threadGroup_->wait();

        std::map<wxString, wxBitmap> output;

        result_.access([&](std::vector<std::pair<std::wstring, ImageHolder>>& r)
        {
            for (auto& [imageName, ih] : r)
            {
                wxImage img(ih.getWidth(), ih.getHeight(), ih.releaseRgb(), false /*static_data*/); //pass ownership
                img.SetAlpha(ih.releaseAlpha(), false /*static_data*/);

                output.emplace(imageName, std::move(img));
            }
        });
        return output;
    }

private:
    const int hqScale_;
    std::vector<wxImage> imgKeeper_;
    Protected<std::vector<std::pair<std::wstring, ImageHolder>>> result_;

    using TaskType = FunctionReturnTypeT<decltype(&getScalerTask)>;
    std::optional<ThreadGroup<TaskType>> threadGroup_{ ThreadGroup<TaskType>(std::max<int>(std::thread::hardware_concurrency(), 1), "xBRZ Scaler") };
    //hardware_concurrency() == 0 if "not computable or well defined"
};

//================================================================================================
//================================================================================================

class GlobalBitmaps
{
public:
    static std::shared_ptr<GlobalBitmaps> instance()
    {
        static FunStatGlobal<GlobalBitmaps> inst;
        inst.initOnce([] { return std::make_unique<GlobalBitmaps>(); });
        assert(runningMainThread()); //wxWidgets is not thread-safe!
        return inst.get();
    }

    GlobalBitmaps() {}
    ~GlobalBitmaps() { assert(bitmaps_.empty() && anims_.empty()); } //don't leave wxWidgets objects for static destruction!

    void init(const Zstring& filePath);
    void cleanup()
    {
        bitmaps_.clear();
        anims_  .clear();
        dpiScaler_.reset();
    }

    const wxBitmap&    getImage    (const wxString& name);
    const wxAnimation& getAnimation(const wxString& name) const;

private:
    GlobalBitmaps           (const GlobalBitmaps&) = delete;
    GlobalBitmaps& operator=(const GlobalBitmaps&) = delete;

    std::map<wxString, wxBitmap> bitmaps_;
    std::map<wxString, wxAnimation> anims_;

    std::unique_ptr<DpiParallelScaler> dpiScaler_;
};


void GlobalBitmaps::init(const Zstring& zipPath)
{
    assert(bitmaps_.empty() && anims_.empty());

    std::vector<std::pair<wxString /*file name*/, std::string /*byte stream*/>> streams;

    try //to load from ZIP first:
    {
        //wxFFileInputStream/wxZipInputStream loads in junks of 512 bytes => WTF!!! => implement sane file loading:
        const std::string rawStream = loadBinContainer<std::string>(zipPath, nullptr /*notifyUnbufferedIO*/); //throw FileError
        wxMemoryInputStream memStream(rawStream.c_str(), rawStream.size()); //does not take ownership
        wxZipInputStream zipStream(memStream, wxConvUTF8);
        //do NOT rely on wxConvLocal! On failure shows unhelpful popup "Cannot convert from the charset 'Unknown encoding (-1)'!"

        while (const auto& entry = std::unique_ptr<wxZipEntry>(zipStream.GetNextEntry())) //take ownership!
            if (std::string stream(entry->GetSize(), '\0'); !stream.empty() && zipStream.ReadAll(&stream[0], stream.size()))
                streams.emplace_back(entry->GetName(), std::move(stream));
            else
                assert(false);
    }
    catch (FileError&) //fall back to folder
    {
        traverseFolder(beforeLast(zipPath, Zstr(".zip"), IF_MISSING_RETURN_NONE), [&](const FileInfo& fi)
        {
            if (endsWith(fi.fullPath, Zstr(".png")))
                try
                {
                    std::string stream = loadBinContainer<std::string>(fi.fullPath, nullptr /*notifyUnbufferedIO*/); //throw FileError
                    streams.emplace_back(utfTo<wxString>(fi.itemName), std::move(stream));
                }
                catch (FileError&) { assert(false); }
        }, nullptr, nullptr, [](const std::wstring& errorMsg) { assert(false); }); //errors are not really critical in this context
    }
    //--------------------------------------------------------------------

    //activate support for .png files
    wxImage::AddHandler(new wxPNGHandler); //ownership passed

    //do we need xBRZ scaling for high quality DPI images?
    const int hqScale = std::clamp<int>(std::ceil(fastFromDIP(1000) / 1000.0), 1, xbrz::SCALE_FACTOR_MAX);
    //even for 125% DPI scaling, "2xBRZ + bilinear downscale" gives a better result than mere "125% bilinear upscale"!
    if (hqScale > 1)
        dpiScaler_ = std::make_unique<DpiParallelScaler>(hqScale);

    for (const auto& [fileName, stream] : streams)
    {
        wxMemoryInputStream wxstream(stream.c_str(), stream.size()); //stream does not take ownership of data
        //bonus: work around wxWidgets bug: wxAnimation::Load() requires seekable input stream (zip-input stream is not seekable)
          
        if (endsWith(fileName, L".png"))
        {
            wxImage img(wxstream, wxBITMAP_TYPE_PNG);
            assert(img.IsOk());

            //end this alpha/no-alpha/mask/wxDC::DrawBitmap/RTL/high-contrast-scheme interoperability nightmare here and now!!!!
            //=> there's only one type of wxImage: with alpha channel, no mask!!!
            convertToVanillaImage(img);

            if (dpiScaler_)
                dpiScaler_->add(fileName, img); //scale in parallel!
            else
                bitmaps_.emplace(fileName, img);
        }
#if 0
        else if (endsWith(name, L".gif"))
        {
            [[maybe_unused]] const bool loadSuccess = anims_[fileName].Load(wxstream, wxANIMATION_TYPE_GIF);
            assert(loadSuccess);
        }
#endif
        else
            assert(false);
    }
}


const wxBitmap& GlobalBitmaps::getImage(const wxString& name)
{
    //test: this function is first called about 220ms after GlobalBitmaps::init() has ended
    //      => should be enough time to finish xBRZ scaling in parallel (which takes 50ms)
    //debug perf: extra 800-1000ms during startup
    if (dpiScaler_)
    {
        bitmaps_ = dpiScaler_->waitAndGetResult();
        dpiScaler_.reset();
    }

    auto it = bitmaps_.find(contains(name, L'.') ? name : name + L".png"); //assume .png ending if nothing else specified
    if (it != bitmaps_.end())
        return it->second;
    assert(false);
    return wxNullBitmap;
}


const wxAnimation& GlobalBitmaps::getAnimation(const wxString& name) const
{
    auto it = anims_.find(contains(name, L'.') ? name : name + L".gif");
    if (it != anims_.end())
        return it->second;
    assert(false);
    return wxNullAnimation;
}
}


void zen::initResourceImages(const Zstring& zipPath)
{
    if (std::shared_ptr<GlobalBitmaps> inst = GlobalBitmaps::instance())
        inst->init(zipPath);
    else
        assert(false);
}


void zen::cleanupResourceImages()
{
    if (std::shared_ptr<GlobalBitmaps> inst = GlobalBitmaps::instance())
        inst->cleanup();
    else
        assert(false);
}


const wxBitmap& zen::getResourceImage(const wxString& name)
{
    if (std::shared_ptr<GlobalBitmaps> inst = GlobalBitmaps::instance())
        return inst->getImage(name);
    assert(false);
    return wxNullBitmap;
}


const wxAnimation& zen::getResourceAnimation(const wxString& name)
{
    if (std::shared_ptr<GlobalBitmaps> inst = GlobalBitmaps::instance())
        return inst->getAnimation(name);
    assert(false);
    return wxNullAnimation;
}
