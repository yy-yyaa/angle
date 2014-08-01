#include "precompiled.h"
//
// Copyright (c) 2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// Image11.h: Implements the rx::Image11 class, which acts as the interface to
// the actual underlying resources of a Texture

#include "libGLESv2/renderer/d3d/d3d11/Renderer11.h"
#include "libGLESv2/renderer/d3d/d3d11/Image11.h"
#include "libGLESv2/renderer/d3d/d3d11/TextureStorage11.h"
#include "libGLESv2/Framebuffer.h"
#include "libGLESv2/FramebufferAttachment.h"

#include "libGLESv2/main.h"
#include "common/utilities.h"
#include "libGLESv2/renderer/d3d/d3d11/formatutils11.h"
#include "libGLESv2/renderer/d3d/d3d11/renderer11_utils.h"

namespace rx
{

Image11::Image11()
{
    mStagingTexture = NULL;
    mRenderer = NULL;
    mDXGIFormat = DXGI_FORMAT_UNKNOWN;
}

Image11::~Image11()
{
    SafeRelease(mStagingTexture);
}

Image11 *Image11::makeImage11(Image *img)
{
    ASSERT(HAS_DYNAMIC_TYPE(rx::Image11*, img));
    return static_cast<rx::Image11*>(img);
}

void Image11::generateMipmap(Image11 *dest, Image11 *src)
{
    ASSERT(src->getDXGIFormat() == dest->getDXGIFormat());
    ASSERT(src->getWidth() == 1 || src->getWidth() / 2 == dest->getWidth());
    ASSERT(src->getHeight() == 1 || src->getHeight() / 2 == dest->getHeight());

    MipGenerationFunction mipFunction = d3d11::GetMipGenerationFunction(src->getDXGIFormat());
    ASSERT(mipFunction != NULL);

    D3D11_MAPPED_SUBRESOURCE destMapped;
    HRESULT destMapResult = dest->map(D3D11_MAP_WRITE, &destMapped);
    if (FAILED(destMapResult))
    {
        ERR("Failed to map destination image for mip map generation. HRESULT:0x%X", destMapResult);
        return;
    }

    D3D11_MAPPED_SUBRESOURCE srcMapped;
    HRESULT srcMapResult = src->map(D3D11_MAP_READ, &srcMapped);
    if (FAILED(srcMapResult))
    {
        ERR("Failed to map source image for mip map generation. HRESULT:0x%X", srcMapResult);

        dest->unmap();
        return;
    }

    const uint8_t *sourceData = reinterpret_cast<const uint8_t*>(srcMapped.pData);
    uint8_t *destData = reinterpret_cast<uint8_t*>(destMapped.pData);

    mipFunction(src->getWidth(), src->getHeight(), src->getDepth(), sourceData, srcMapped.RowPitch, srcMapped.DepthPitch,
                destData, destMapped.RowPitch, destMapped.DepthPitch);

    dest->unmap();
    src->unmap();

    dest->markDirty();
}

bool Image11::isDirty() const
{
    // Make sure that this image is marked as dirty even if the staging texture hasn't been created yet
    // if initialization is required before use.
    return (mDirty && (mStagingTexture || gl_d3d11::RequiresTextureDataInitialization(mInternalFormat)));
}

bool Image11::copyToStorage(TextureStorageInterface2D *storage, int level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height)
{
    TextureStorage11_2D *storage11 = TextureStorage11_2D::makeTextureStorage11_2D(storage->getStorageInstance());
    return storage11->updateSubresourceLevel(getStagingTexture(), getStagingSubresource(), level, 0, xoffset, yoffset, 0, width, height, 1);
}

bool Image11::copyToStorage(TextureStorageInterfaceCube *storage, int face, int level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height)
{
    TextureStorage11_Cube *storage11 = TextureStorage11_Cube::makeTextureStorage11_Cube(storage->getStorageInstance());
    return storage11->updateSubresourceLevel(getStagingTexture(), getStagingSubresource(), level, face, xoffset, yoffset, 0, width, height, 1);
}

bool Image11::copyToStorage(TextureStorageInterface3D *storage, int level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth)
{
    TextureStorage11_3D *storage11 = TextureStorage11_3D::makeTextureStorage11_3D(storage->getStorageInstance());
    return storage11->updateSubresourceLevel(getStagingTexture(), getStagingSubresource(), level, 0, xoffset, yoffset, zoffset, width, height, depth);
}

bool Image11::copyToStorage(TextureStorageInterface2DArray *storage, int level, GLint xoffset, GLint yoffset, GLint arrayLayer, GLsizei width, GLsizei height)
{
    TextureStorage11_2DArray *storage11 = TextureStorage11_2DArray::makeTextureStorage11_2DArray(storage->getStorageInstance());
    return storage11->updateSubresourceLevel(getStagingTexture(), getStagingSubresource(), level, arrayLayer, xoffset, yoffset, 0, width, height, 1);
}

bool Image11::redefine(Renderer *renderer, GLenum target, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth, bool forceRelease)
{
    if (mWidth != width ||
        mHeight != height ||
        mInternalFormat != internalformat ||
        forceRelease)
    {
        mRenderer = Renderer11::makeRenderer11(renderer);

        mWidth = width;
        mHeight = height;
        mDepth = depth;
        mInternalFormat = internalformat;
        mTarget = target;

        // compute the d3d format that will be used
        mDXGIFormat = gl_d3d11::GetTexFormat(internalformat);
        mActualFormat = d3d11_gl::GetInternalFormat(mDXGIFormat);
        mRenderable = gl_d3d11::GetRTVFormat(internalformat) != DXGI_FORMAT_UNKNOWN;

        SafeRelease(mStagingTexture);
        mDirty = gl_d3d11::RequiresTextureDataInitialization(mInternalFormat);

        return true;
    }

    return false;
}

DXGI_FORMAT Image11::getDXGIFormat() const
{
    // this should only happen if the image hasn't been redefined first
    // which would be a bug by the caller
    ASSERT(mDXGIFormat != DXGI_FORMAT_UNKNOWN);

    return mDXGIFormat;
}

// Store the pixel rectangle designated by xoffset,yoffset,width,height with pixels stored as format/type at input
// into the target pixel rectangle.
void Image11::loadData(GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                       GLint unpackAlignment, GLenum type, const void *input)
{
    const gl::InternalFormat &formatInfo = gl::GetInternalFormatInfo(mInternalFormat);
    GLsizei inputRowPitch = formatInfo.computeRowPitch(type, width, unpackAlignment);
    GLsizei inputDepthPitch = formatInfo.computeDepthPitch(type, width, height, unpackAlignment);
    GLuint outputPixelSize = d3d11::GetFormatPixelBytes(mDXGIFormat);

    LoadImageFunction loadFunction = d3d11::GetImageLoadFunction(mInternalFormat, type);
    ASSERT(loadFunction != NULL);

    D3D11_MAPPED_SUBRESOURCE mappedImage;
    HRESULT result = map(D3D11_MAP_WRITE, &mappedImage);
    if (FAILED(result))
    {
        ERR("Could not map image for loading.");
        return;
    }

    uint8_t* offsetMappedData = (reinterpret_cast<uint8_t*>(mappedImage.pData) + (yoffset * mappedImage.RowPitch + xoffset * outputPixelSize + zoffset * mappedImage.DepthPitch));
    loadFunction(width, height, depth,
                 reinterpret_cast<const uint8_t*>(input), inputRowPitch, inputDepthPitch,
                 offsetMappedData, mappedImage.RowPitch, mappedImage.DepthPitch);

    unmap();
}

void Image11::loadCompressedData(GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth,
                                 const void *input)
{
    const gl::InternalFormat &formatInfo = gl::GetInternalFormatInfo(mInternalFormat);
    GLsizei inputRowPitch = formatInfo.computeRowPitch(GL_UNSIGNED_BYTE, width, 1);
    GLsizei inputDepthPitch = formatInfo.computeDepthPitch(GL_UNSIGNED_BYTE, width, height, 1);

    GLuint outputPixelSize = d3d11::GetFormatPixelBytes(mDXGIFormat);
    GLuint outputBlockWidth = d3d11::GetBlockWidth(mDXGIFormat);
    GLuint outputBlockHeight = d3d11::GetBlockHeight(mDXGIFormat);

    ASSERT(xoffset % outputBlockWidth == 0);
    ASSERT(yoffset % outputBlockHeight == 0);

    LoadImageFunction loadFunction = d3d11::GetImageLoadFunction(mInternalFormat, GL_UNSIGNED_BYTE);
    ASSERT(loadFunction != NULL);

    D3D11_MAPPED_SUBRESOURCE mappedImage;
    HRESULT result = map(D3D11_MAP_WRITE, &mappedImage);
    if (FAILED(result))
    {
        ERR("Could not map image for loading.");
        return;
    }

    uint8_t* offsetMappedData = reinterpret_cast<uint8_t*>(mappedImage.pData) + ((yoffset / outputBlockHeight) * mappedImage.RowPitch +
                                                                           (xoffset / outputBlockWidth) * outputPixelSize +
                                                                           zoffset * mappedImage.DepthPitch);

    loadFunction(width, height, depth,
                 reinterpret_cast<const uint8_t*>(input), inputRowPitch, inputDepthPitch,
                 offsetMappedData, mappedImage.RowPitch, mappedImage.DepthPitch);

    unmap();
}

void Image11::copy(GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height, gl::Framebuffer *source)
{
    gl::FramebufferAttachment *colorbuffer = source->getReadColorbuffer();

    if (colorbuffer && colorbuffer->getActualFormat() == mActualFormat)
    {
        // No conversion needed-- use copyback fastpath
        ID3D11Texture2D *colorBufferTexture = NULL;
        unsigned int subresourceIndex = 0;

        if (mRenderer->getRenderTargetResource(colorbuffer, &subresourceIndex, &colorBufferTexture))
        {
            D3D11_TEXTURE2D_DESC textureDesc;
            colorBufferTexture->GetDesc(&textureDesc);

            ID3D11Device *device = mRenderer->getDevice();
            ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();

            ID3D11Texture2D* srcTex = NULL;
            if (textureDesc.SampleDesc.Count > 1)
            {
                D3D11_TEXTURE2D_DESC resolveDesc;
                resolveDesc.Width = textureDesc.Width;
                resolveDesc.Height = textureDesc.Height;
                resolveDesc.MipLevels = 1;
                resolveDesc.ArraySize = 1;
                resolveDesc.Format = textureDesc.Format;
                resolveDesc.SampleDesc.Count = 1;
                resolveDesc.SampleDesc.Quality = 0;
                resolveDesc.Usage = D3D11_USAGE_DEFAULT;
                resolveDesc.BindFlags = 0;
                resolveDesc.CPUAccessFlags = 0;
                resolveDesc.MiscFlags = 0;

                HRESULT result = device->CreateTexture2D(&resolveDesc, NULL, &srcTex);
                if (FAILED(result))
                {
                    ERR("Failed to create resolve texture for Image11::copy, HRESULT: 0x%X.", result);
                    return;
                }

                deviceContext->ResolveSubresource(srcTex, 0, colorBufferTexture, subresourceIndex, textureDesc.Format);
                subresourceIndex = 0;
            }
            else
            {
                srcTex = colorBufferTexture;
                srcTex->AddRef();
            }

            D3D11_BOX srcBox;
            srcBox.left = x;
            srcBox.right = x + width;
            srcBox.top = y;
            srcBox.bottom = y + height;
            srcBox.front = 0;
            srcBox.back = 1;

            deviceContext->CopySubresourceRegion(mStagingTexture, 0, xoffset, yoffset, zoffset, srcTex, subresourceIndex, &srcBox);

            SafeRelease(srcTex);
            SafeRelease(colorBufferTexture);
        }
    }
    else
    {
        // This format requires conversion, so we must copy the texture to staging and manually convert via readPixels
        D3D11_MAPPED_SUBRESOURCE mappedImage;
        HRESULT result = map(D3D11_MAP_WRITE, &mappedImage);
        if (FAILED(result))
        {
            ERR("Failed to map texture for Image11::copy, HRESULT: 0x%X.", result);
            return;
        }

        // determine the offset coordinate into the destination buffer
        GLsizei rowOffset = gl::GetInternalFormatInfo(mActualFormat).pixelBytes * xoffset;
        void *dataOffset = static_cast<unsigned char*>(mappedImage.pData) + mappedImage.RowPitch * yoffset + rowOffset + zoffset * mappedImage.DepthPitch;

        const gl::InternalFormat &formatInfo = gl::GetInternalFormatInfo(mInternalFormat);

        mRenderer->readPixels(source, x, y, width, height, formatInfo.format, formatInfo.type, mappedImage.RowPitch, gl::PixelPackState(), dataOffset);

        unmap();
    }
}

ID3D11Resource *Image11::getStagingTexture()
{
    createStagingTexture();

    return mStagingTexture;
}

unsigned int Image11::getStagingSubresource()
{
    createStagingTexture();

    return mStagingSubresource;
}

void Image11::createStagingTexture()
{
    if (mStagingTexture)
    {
        return;
    }

    const DXGI_FORMAT dxgiFormat = getDXGIFormat();

    if (mWidth > 0 && mHeight > 0 && mDepth > 0)
    {
        ID3D11Device *device = mRenderer->getDevice();
        HRESULT result;

        int lodOffset = 1;
        GLsizei width = mWidth;
        GLsizei height = mHeight;

        // adjust size if needed for compressed textures
        d3d11::MakeValidSize(false, dxgiFormat, &width, &height, &lodOffset);

        if (mTarget == GL_TEXTURE_3D)
        {
            ID3D11Texture3D *newTexture = NULL;

            D3D11_TEXTURE3D_DESC desc;
            desc.Width = width;
            desc.Height = height;
            desc.Depth = mDepth;
            desc.MipLevels = lodOffset + 1;
            desc.Format = dxgiFormat;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;

            if (gl_d3d11::RequiresTextureDataInitialization(mInternalFormat))
            {
                std::vector<D3D11_SUBRESOURCE_DATA> initialData;
                std::vector< std::vector<BYTE> > textureData;
                d3d11::GenerateInitialTextureData(mInternalFormat, width, height, mDepth,
                                                  lodOffset + 1, &initialData, &textureData);

                result = device->CreateTexture3D(&desc, initialData.data(), &newTexture);
            }
            else
            {
                result = device->CreateTexture3D(&desc, NULL, &newTexture);
            }

            if (FAILED(result))
            {
                ASSERT(result == E_OUTOFMEMORY);
                ERR("Creating image failed.");
                return gl::error(GL_OUT_OF_MEMORY);
            }

            mStagingTexture = newTexture;
            mStagingSubresource = D3D11CalcSubresource(lodOffset, 0, lodOffset + 1);
        }
        else if (mTarget == GL_TEXTURE_2D || mTarget == GL_TEXTURE_2D_ARRAY || mTarget == GL_TEXTURE_CUBE_MAP)
        {
            ID3D11Texture2D *newTexture = NULL;

            D3D11_TEXTURE2D_DESC desc;
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = lodOffset + 1;
            desc.ArraySize = 1;
            desc.Format = dxgiFormat;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;

            if (gl_d3d11::RequiresTextureDataInitialization(mInternalFormat))
            {
                std::vector<D3D11_SUBRESOURCE_DATA> initialData;
                std::vector< std::vector<BYTE> > textureData;
                d3d11::GenerateInitialTextureData(mInternalFormat, width, height, 1,
                                                  lodOffset + 1, &initialData, &textureData);

                result = device->CreateTexture2D(&desc, initialData.data(), &newTexture);
            }
            else
            {
                result = device->CreateTexture2D(&desc, NULL, &newTexture);
            }

            if (FAILED(result))
            {
                ASSERT(result == E_OUTOFMEMORY);
                ERR("Creating image failed.");
                return gl::error(GL_OUT_OF_MEMORY);
            }

            mStagingTexture = newTexture;
            mStagingSubresource = D3D11CalcSubresource(lodOffset, 0, lodOffset + 1);
        }
        else
        {
            UNREACHABLE();
        }
    }

    mDirty = false;
}

HRESULT Image11::map(D3D11_MAP mapType, D3D11_MAPPED_SUBRESOURCE *map)
{
    createStagingTexture();

    HRESULT result = E_FAIL;

    if (mStagingTexture)
    {
        ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
        result = deviceContext->Map(mStagingTexture, mStagingSubresource, mapType, 0, map);

        // this can fail if the device is removed (from TDR)
        if (d3d11::isDeviceLostError(result))
        {
            mRenderer->notifyDeviceLost();
        }
        else if (SUCCEEDED(result))
        {
            mDirty = true;
        }
    }

    return result;
}

void Image11::unmap()
{
    if (mStagingTexture)
    {
        ID3D11DeviceContext *deviceContext = mRenderer->getDeviceContext();
        deviceContext->Unmap(mStagingTexture, mStagingSubresource);
    }
}

}