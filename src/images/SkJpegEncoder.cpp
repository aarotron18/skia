/*
 * Copyright 2007 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "include/core/SkTypes.h"

#ifdef SK_ENCODE_JPEG

#include "include/core/SkAlphaType.h"
#include "include/core/SkColorType.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRefCnt.h"
#include "include/encode/SkEncoder.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/private/base/SkNoncopyable.h"
#include "include/private/base/SkTemplates.h"
#include "src/base/SkMSAN.h"
#include "src/codec/SkJpegPriv.h"
#include "src/images/SkImageEncoderFns.h"
#include "src/images/SkImageEncoderPriv.h"
#include "src/images/SkJPEGWriteUtility.h"

#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

extern "C" {
    #include "jpeglib.h"
    #include "jmorecfg.h"
}

class SkWStream;

class SkJpegEncoderMgr final : SkNoncopyable {
public:
    /*
     * Create the decode manager
     * Does not take ownership of stream or suffix.
     */
    static std::unique_ptr<SkJpegEncoderMgr> Make(SkWStream* stream, SkData* suffix) {
        return std::unique_ptr<SkJpegEncoderMgr>(new SkJpegEncoderMgr(stream, suffix));
    }

    bool setParams(const SkImageInfo& srcInfo, const SkJpegEncoder::Options& options);

    jpeg_compress_struct* cinfo() { return &fCInfo; }

    skjpeg_error_mgr* errorMgr() { return &fErrMgr; }

    transform_scanline_proc proc() const { return fProc; }

    ~SkJpegEncoderMgr() {
        jpeg_destroy_compress(&fCInfo);
    }

private:
    SkJpegEncoderMgr(SkWStream* stream, SkData* suffix) : fDstMgr(stream, suffix), fProc(nullptr) {
        fCInfo.err = jpeg_std_error(&fErrMgr);
        fErrMgr.error_exit = skjpeg_error_exit;
        jpeg_create_compress(&fCInfo);
        fCInfo.dest = &fDstMgr;
    }

    jpeg_compress_struct    fCInfo;
    skjpeg_error_mgr        fErrMgr;
    skjpeg_destination_mgr  fDstMgr;
    transform_scanline_proc fProc;
};

bool SkJpegEncoderMgr::setParams(const SkImageInfo& srcInfo, const SkJpegEncoder::Options& options)
{
    auto chooseProc8888 = [&]() {
        if (kUnpremul_SkAlphaType == srcInfo.alphaType() &&
                options.fAlphaOption == SkJpegEncoder::AlphaOption::kBlendOnBlack) {
            return transform_scanline_to_premul_legacy;
        }
        return (transform_scanline_proc) nullptr;
    };

    J_COLOR_SPACE jpegColorType = JCS_EXT_RGBA;
    int numComponents = 0;
    switch (srcInfo.colorType()) {
        case kRGBA_8888_SkColorType:
            fProc = chooseProc8888();
            jpegColorType = JCS_EXT_RGBA;
            numComponents = 4;
            break;
        case kBGRA_8888_SkColorType:
            fProc = chooseProc8888();
            jpegColorType = JCS_EXT_BGRA;
            numComponents = 4;
            break;
        case kRGB_565_SkColorType:
            fProc = transform_scanline_565;
            jpegColorType = JCS_RGB;
            numComponents = 3;
            break;
        case kARGB_4444_SkColorType:
            if (SkJpegEncoder::AlphaOption::kBlendOnBlack == options.fAlphaOption) {
                return false;
            }

            fProc = transform_scanline_444;
            jpegColorType = JCS_RGB;
            numComponents = 3;
            break;
        case kGray_8_SkColorType:
            SkASSERT(srcInfo.isOpaque());
            jpegColorType = JCS_GRAYSCALE;
            numComponents = 1;
            break;
        case kRGBA_F16_SkColorType:
            if (kUnpremul_SkAlphaType == srcInfo.alphaType() &&
                    options.fAlphaOption == SkJpegEncoder::AlphaOption::kBlendOnBlack) {
                fProc = transform_scanline_F16_to_premul_8888;
            } else {
                fProc = transform_scanline_F16_to_8888;
            }
            jpegColorType = JCS_EXT_RGBA;
            numComponents = 4;
            break;
        default:
            return false;
    }

    fCInfo.image_width = srcInfo.width();
    fCInfo.image_height = srcInfo.height();
    fCInfo.in_color_space = jpegColorType;
    fCInfo.input_components = numComponents;
    jpeg_set_defaults(&fCInfo);

    if (kGray_8_SkColorType != srcInfo.colorType()) {
        switch (options.fDownsample) {
            case SkJpegEncoder::Downsample::k420:
                SkASSERT(2 == fCInfo.comp_info[0].h_samp_factor);
                SkASSERT(2 == fCInfo.comp_info[0].v_samp_factor);
                SkASSERT(1 == fCInfo.comp_info[1].h_samp_factor);
                SkASSERT(1 == fCInfo.comp_info[1].v_samp_factor);
                SkASSERT(1 == fCInfo.comp_info[2].h_samp_factor);
                SkASSERT(1 == fCInfo.comp_info[2].v_samp_factor);
                break;
            case SkJpegEncoder::Downsample::k422:
                fCInfo.comp_info[0].h_samp_factor = 2;
                fCInfo.comp_info[0].v_samp_factor = 1;
                fCInfo.comp_info[1].h_samp_factor = 1;
                fCInfo.comp_info[1].v_samp_factor = 1;
                fCInfo.comp_info[2].h_samp_factor = 1;
                fCInfo.comp_info[2].v_samp_factor = 1;
                break;
            case SkJpegEncoder::Downsample::k444:
                fCInfo.comp_info[0].h_samp_factor = 1;
                fCInfo.comp_info[0].v_samp_factor = 1;
                fCInfo.comp_info[1].h_samp_factor = 1;
                fCInfo.comp_info[1].v_samp_factor = 1;
                fCInfo.comp_info[2].h_samp_factor = 1;
                fCInfo.comp_info[2].v_samp_factor = 1;
                break;
        }
    }

    // Tells libjpeg-turbo to compute optimal Huffman coding tables
    // for the image.  This improves compression at the cost of
    // slower encode performance.
    fCInfo.optimize_coding = TRUE;
    return true;
}

std::unique_ptr<SkEncoder> SkJpegEncoder::Make(SkWStream* dst, const SkPixmap& src,
                                               const Options& options) {
    return Make(dst, src, options, 0, nullptr, nullptr, nullptr);
}

std::unique_ptr<SkEncoder> SkJpegEncoder::Make(SkWStream* dst,
                                               const SkPixmap& src,
                                               const Options& options,
                                               size_t segmentCount,
                                               uint8_t* segmentMarkers,
                                               SkData** segmentData,
                                               SkData* suffix) {
    if (!SkPixmapIsValid(src)) {
        return nullptr;
    }

    std::unique_ptr<SkJpegEncoderMgr> encoderMgr = SkJpegEncoderMgr::Make(dst, suffix);

    skjpeg_error_mgr::AutoPushJmpBuf jmp(encoderMgr->errorMgr());
    if (setjmp(jmp)) {
        return nullptr;
    }

    if (!encoderMgr->setParams(src.info(), options)) {
        return nullptr;
    }

    jpeg_set_quality(encoderMgr->cinfo(), options.fQuality, TRUE);
    jpeg_start_compress(encoderMgr->cinfo(), TRUE);

    for (size_t i = 0; i < segmentCount; ++i) {
        SkASSERT(segmentData[i]->size() <= kSegmentDataMaxSize);
        jpeg_write_marker(encoderMgr->cinfo(),
                          segmentMarkers[i],
                          segmentData[i]->bytes(),
                          segmentData[i]->size());
    }

    sk_sp<SkData> icc =
            icc_from_color_space(src.info(), options.fICCProfile, options.fICCProfileDescription);
    if (icc) {
        // Create a contiguous block of memory with the icc signature followed by the profile.
        sk_sp<SkData> markerData =
                SkData::MakeUninitialized(kICCMarkerHeaderSize + icc->size());
        uint8_t* ptr = (uint8_t*) markerData->writable_data();
        memcpy(ptr, kICCSig, sizeof(kICCSig));
        ptr += sizeof(kICCSig);
        *ptr++ = 1; // This is the first marker.
        *ptr++ = 1; // Out of one total markers.
        memcpy(ptr, icc->data(), icc->size());

        jpeg_write_marker(encoderMgr->cinfo(), kICCMarker, markerData->bytes(), markerData->size());
    }

    return std::unique_ptr<SkJpegEncoder>(new SkJpegEncoder(std::move(encoderMgr), src));
}

SkJpegEncoder::SkJpegEncoder(std::unique_ptr<SkJpegEncoderMgr> encoderMgr, const SkPixmap& src)
    : INHERITED(src, encoderMgr->proc() ? encoderMgr->cinfo()->input_components*src.width() : 0)
    , fEncoderMgr(std::move(encoderMgr))
{}

SkJpegEncoder::~SkJpegEncoder() {}

bool SkJpegEncoder::onEncodeRows(int numRows) {
    skjpeg_error_mgr::AutoPushJmpBuf jmp(fEncoderMgr->errorMgr());
    if (setjmp(jmp)) {
        return false;
    }

    const size_t srcBytes = SkColorTypeBytesPerPixel(fSrc.colorType()) * fSrc.width();
    const size_t jpegSrcBytes = fEncoderMgr->cinfo()->input_components * fSrc.width();

    const void* srcRow = fSrc.addr(0, fCurrRow);
    for (int i = 0; i < numRows; i++) {
        JSAMPLE* jpegSrcRow = (JSAMPLE*) srcRow;
        if (fEncoderMgr->proc()) {
            sk_msan_assert_initialized(srcRow, SkTAddOffset<const void>(srcRow, srcBytes));
            fEncoderMgr->proc()((char*)fStorage.get(),
                                (const char*)srcRow,
                                fSrc.width(),
                                fEncoderMgr->cinfo()->input_components);
            jpegSrcRow = fStorage.get();
            sk_msan_assert_initialized(jpegSrcRow,
                                       SkTAddOffset<const void>(jpegSrcRow, jpegSrcBytes));
        } else {
            // Same as above, but this repetition allows determining whether a
            // proc was used when msan asserts.
            sk_msan_assert_initialized(jpegSrcRow,
                                       SkTAddOffset<const void>(jpegSrcRow, jpegSrcBytes));
        }

        jpeg_write_scanlines(fEncoderMgr->cinfo(), &jpegSrcRow, 1);
        srcRow = SkTAddOffset<const void>(srcRow, fSrc.rowBytes());
    }

    fCurrRow += numRows;
    if (fCurrRow == fSrc.height()) {
        jpeg_finish_compress(fEncoderMgr->cinfo());
    }

    return true;
}

bool SkJpegEncoder::Encode(SkWStream* dst, const SkPixmap& src, const Options& options) {
    auto encoder = SkJpegEncoder::Make(dst, src, options);
    return encoder.get() && encoder->encodeRows(src.height());
}

#endif
