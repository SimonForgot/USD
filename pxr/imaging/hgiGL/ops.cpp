//
// Copyright 2020 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/imaging/hgiGL/ops.h"
#include "pxr/imaging/hgiGL/buffer.h"
#include "pxr/imaging/hgiGL/conversions.h"
#include "pxr/imaging/hgiGL/diagnostic.h"
#include "pxr/imaging/hgiGL/graphicsEncoder.h"
#include "pxr/imaging/hgiGL/pipeline.h"
#include "pxr/imaging/hgiGL/resourceBindings.h"
#include "pxr/imaging/hgiGL/texture.h"

PXR_NAMESPACE_OPEN_SCOPE


HgiGLOpsFn
HgiGLOps::PushDebugGroup(const char* label)
{
    return [label] {
        #if defined(GL_KHR_debug)
        if (GLEW_KHR_debug) {
            glPushDebugGroup(GL_DEBUG_SOURCE_THIRD_PARTY, 0, -1, label);
        }
        #endif
    };
}

HgiGLOpsFn
HgiGLOps::PopDebugGroup()
{
    return [] {
        #if defined(GL_KHR_debug)
        if (GLEW_KHR_debug) {
            glPopDebugGroup();
        }
        #endif
    };
}

HgiGLOpsFn
HgiGLOps::CopyTextureGpuToCpu(HgiTextureGpuToCpuOp const& copyOp)
{
    return [copyOp] {
        HgiTextureHandle texHandle = copyOp.gpuSourceTexture;
        HgiGLTexture* srcTexture = static_cast<HgiGLTexture*>(texHandle.Get());

        if (!TF_VERIFY(srcTexture && srcTexture->GetTextureId(),
            "Invalid texture handle")) {
            return;
        }

        if (copyOp.destinationBufferByteSize == 0) {
            TF_WARN("The size of the data to copy was zero (aborted)");
            return;
        }

        HgiTextureDesc const& texDesc = srcTexture->GetDescriptor();

        uint32_t layerCnt = copyOp.startLayer + copyOp.numLayers;
        if (!TF_VERIFY(texDesc.layerCount >= layerCnt,
            "Texture has less layers than attempted to be copied")) {
            return;
        }

        GLenum glInternalFormat = 0;
        GLenum glFormat = 0;
        GLenum glPixelType = 0;

        if (texDesc.usage & HgiTextureUsageBitsColorTarget) {
            HgiGLConversions::GetFormat(
                texDesc.format,
                &glFormat,
                &glPixelType,
                &glInternalFormat);
        } else if (texDesc.usage & HgiTextureUsageBitsDepthTarget) {
            TF_VERIFY(texDesc.format == HgiFormatFloat32);
            glFormat = GL_DEPTH_COMPONENT;
            glPixelType = GL_FLOAT;
            glInternalFormat = GL_DEPTH_COMPONENT32F;
        } else {
            TF_CODING_ERROR("Unknown HgTextureUsage bit");
        }

        // Make sure writes are finished before we read from the texture
        glMemoryBarrier(GL_ALL_BARRIER_BITS);

        glGetTextureSubImage(
            srcTexture->GetTextureId(),
            copyOp.mipLevel,
            copyOp.sourceTexelOffset[0], // x offset
            copyOp.sourceTexelOffset[1], // y offset
            copyOp.sourceTexelOffset[2], // z offset
            texDesc.dimensions[0], // width
            texDesc.dimensions[1], // height
            texDesc.dimensions[2], // layerCnt
            glFormat,
            glPixelType,
            copyOp.destinationBufferByteSize,
            copyOp.cpuDestinationBuffer);

        HGIGL_POST_PENDING_GL_ERRORS();
    };
}

HgiGLOpsFn 
HgiGLOps::CopyBufferCpuToGpu(HgiBufferCpuToGpuOp const& copyOp)
{
    return [copyOp] {
        if (copyOp.byteSize == 0 ||
            !copyOp.cpuSourceBuffer ||
            !copyOp.gpuDestinationBuffer)
        {
            return;
        }

        HgiGLBuffer* glBuffer = static_cast<HgiGLBuffer*>(
            copyOp.gpuDestinationBuffer.Get());

        // Offset into the src buffer
        const char* src = ((const char*) copyOp.cpuSourceBuffer) +
            copyOp.sourceByteOffset;

        // Offset into the dst buffer
        GLintptr dstOffset = copyOp.destinationByteOffset;

        glNamedBufferSubData(
            glBuffer->GetBufferId(),
            dstOffset,
            copyOp.byteSize,
            src);

        // Make sure the copy is finished before reads from buffer.
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    };
}

HgiGLOpsFn
HgiGLOps::ResolveImage(HgiResolveImageOp const& resolveOp)
{
    return [resolveOp] {
        // Create framebuffers for resolve.
        uint32_t readFramebuffer;
        uint32_t writeFramebuffer;
        glCreateFramebuffers(1, &readFramebuffer);
        glCreateFramebuffers(1, &writeFramebuffer);

        // Gather source and destination textures
        HgiGLTexture* glSrcTexture = static_cast<HgiGLTexture*>(
            resolveOp.source.Get());
        HgiGLTexture* glDstTexture = static_cast<HgiGLTexture*>(
            resolveOp.destination.Get());

        if (!glSrcTexture || !glDstTexture) {
            TF_CODING_ERROR("No textures provided for resolve");
            return;
        }

        uint32_t readAttachment = glSrcTexture->GetTextureId();
        TF_VERIFY(glIsTexture(readAttachment), "Source is not a texture");
        uint32_t writeAttachment = glDstTexture->GetTextureId();
        TF_VERIFY(glIsTexture(writeAttachment), "Destination is not a texture");

        // Update framebuffer bindings
        if (resolveOp.usage & HgiTextureUsageBitsDepthTarget) {
            // Depth-only, so no color attachments for read or write
            // Clear previous color attachment since all attachments must be
            // written to from fragment shader or texels will be undefined.
            GLenum drawBufs[1] = {GL_NONE};
            glNamedFramebufferDrawBuffers(
                readFramebuffer, 1, drawBufs);
            glNamedFramebufferDrawBuffers(
                writeFramebuffer, 1, drawBufs);

            glNamedFramebufferTexture(
                readFramebuffer, GL_COLOR_ATTACHMENT0, 0, /*level*/0);
            glNamedFramebufferTexture(
                writeFramebuffer, GL_COLOR_ATTACHMENT0, 0, /*level*/0);

            glNamedFramebufferTexture(
                readFramebuffer,
                GL_DEPTH_ATTACHMENT,
                readAttachment,
                /*level*/ 0);
            glNamedFramebufferTexture(
                writeFramebuffer,
                GL_DEPTH_ATTACHMENT,
                writeAttachment,
                /*level*/ 0);
        } else {
            // Color-only, so no depth attachments for read or write.
            // Clear previous depth attachment since all attachments must be
            // written to from fragment shader or texels will be undefined.
            GLenum drawBufs[1] = {GL_COLOR_ATTACHMENT0};
            glNamedFramebufferDrawBuffers(
                readFramebuffer, 1, drawBufs);
            glNamedFramebufferDrawBuffers(
                writeFramebuffer, 1, drawBufs);

            glNamedFramebufferTexture(
                readFramebuffer, GL_DEPTH_ATTACHMENT, 0, /*level*/0);
            glNamedFramebufferTexture(
                writeFramebuffer, GL_DEPTH_ATTACHMENT, 0, /*level*/0);

            glNamedFramebufferTexture(
                readFramebuffer,
                GL_COLOR_ATTACHMENT0,
                readAttachment,
                /*level*/ 0);
            glNamedFramebufferTexture(
                writeFramebuffer,
                GL_COLOR_ATTACHMENT0,
                writeAttachment,
                /*level*/ 0);
        }

        GLenum status = glCheckNamedFramebufferStatus(readFramebuffer,
                                                      GL_READ_FRAMEBUFFER);
        TF_VERIFY(status == GL_FRAMEBUFFER_COMPLETE);

        status = glCheckNamedFramebufferStatus(writeFramebuffer,
                                               GL_DRAW_FRAMEBUFFER);
        TF_VERIFY(status == GL_FRAMEBUFFER_COMPLETE);

        // Resolve MSAA fbo to a regular fbo
        GLbitfield mask = (resolveOp.usage & HgiTextureUsageBitsDepthTarget) ?
                GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT;

        const GfVec4i& src = resolveOp.sourceRegion;
        const GfVec4i& dst = resolveOp.destinationRegion;

        // Bind resolve framebuffer
        GLint restoreRead, restoreWrite;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &restoreRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &restoreWrite);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer); // MS
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, writeFramebuffer);// regular

        glBlitFramebuffer(
            src[0], src[1], src[2], src[3],
            dst[0], dst[1], dst[2], dst[3],
            mask,
            GL_NEAREST);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, restoreRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, restoreWrite);

        glDeleteFramebuffers(1, &readFramebuffer);
        glDeleteFramebuffers(1, &writeFramebuffer);

        HGIGL_POST_PENDING_GL_ERRORS();
    };
}

HgiGLOpsFn
HgiGLOps::SetViewport(GfVec4i const& vp)
{
    return [vp] {
        glViewport(vp[0], vp[1], vp[2], vp[3]);
    };
}

HgiGLOpsFn
HgiGLOps::SetScissor(GfVec4i const& sc)
{
    return [sc] {
        glScissor(sc[0], sc[1], sc[2], sc[3]);
    };
}

HgiGLOpsFn
HgiGLOps::BindPipeline(HgiPipelineHandle pipeline)
{
    return [pipeline] {
        if (HgiGLPipeline* p = static_cast<HgiGLPipeline*>(pipeline.Get())) {
            p->BindPipeline();
        }
    };
}

HgiGLOpsFn
HgiGLOps::BindResources(HgiResourceBindingsHandle res)
{
    return [res] {
        if (HgiGLResourceBindings* rb =
            static_cast<HgiGLResourceBindings*>(res.Get()))
        {
            rb->BindResources();
        }
    };
}

HgiGLOpsFn
HgiGLOps::BindVertexBuffers(
    uint32_t firstBinding,
    HgiBufferHandleVector const& vertexBuffers,
    std::vector<uint32_t> const& byteOffsets)
{
    return [firstBinding, vertexBuffers, byteOffsets] {
        TF_VERIFY(byteOffsets.size() == vertexBuffers.size());
        TF_VERIFY(byteOffsets.size() == vertexBuffers.size());

        // XXX use glBindVertexBuffers to bind all VBs in one go.
        for (size_t i=0; i<vertexBuffers.size(); i++) {
            HgiBufferHandle bufHandle = vertexBuffers[i];
            HgiGLBuffer* buf = static_cast<HgiGLBuffer*>(bufHandle.Get());
            HgiBufferDesc const& desc = buf->GetDescriptor();

            TF_VERIFY(desc.usage & HgiBufferUsageVertex);

            glBindVertexBuffer(
                firstBinding + i,
                buf->GetBufferId(),
                byteOffsets[i],
                desc.vertexStride);
        }

        HGIGL_POST_PENDING_GL_ERRORS();
    };
}

HgiGLOpsFn
HgiGLOps::DrawIndexed(
    HgiBufferHandle const& indexBuffer,
    uint32_t indexCount,
    uint32_t indexBufferByteOffset,
    uint32_t vertexOffset,
    uint32_t instanceCount,
    uint32_t firstInstance)
{
    return [indexBuffer, indexCount, indexBufferByteOffset,
        vertexOffset, instanceCount, firstInstance] {
        TF_VERIFY(instanceCount>0);

        HgiGLBuffer* indexBuf = static_cast<HgiGLBuffer*>(indexBuffer.Get());
        HgiBufferDesc const& indexDesc = indexBuf->GetDescriptor();

        // We assume 32bit indices: GL_UNSIGNED_INT
        TF_VERIFY(indexDesc.usage & HgiBufferUsageIndex32);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuf->GetBufferId());

        glDrawElementsInstancedBaseVertex(
            GL_TRIANGLES, // XXX GL_PATCHES for tessellation
            indexCount,
            GL_UNSIGNED_INT,
            (void*)(uintptr_t(indexBufferByteOffset)),
            instanceCount,
            vertexOffset);

        HGIGL_POST_PENDING_GL_ERRORS();
    };
}

HgiGLOpsFn
HgiGLOps::BindFramebufferOp(
    HgiGLDevice* device,
    HgiGraphicsEncoderDesc const& desc)
{
    return [device, desc] {
        TF_VERIFY(desc.HasAttachments(), "Missing attachments");

        uint32_t framebuffer = device->AcquireFramebuffer(desc);

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

        bool blendEnabled = false;

        // Apply LoadOps and blend mode
        for (size_t i=0; i<desc.colorAttachmentDescs.size(); i++) {
            HgiAttachmentDesc const& colorAttachment =
                desc.colorAttachmentDescs[i];

            if (colorAttachment.loadOp == HgiAttachmentLoadOpClear) {
                glClearBufferfv(GL_COLOR, i, colorAttachment.clearValue.data());
            }

            blendEnabled |= colorAttachment.blendEnabled;

            GLenum srcColor = HgiGLConversions::GetBlendFactor(
                colorAttachment.srcColorBlendFactor);
            GLenum dstColor = HgiGLConversions::GetBlendFactor(
                colorAttachment.dstColorBlendFactor);

            GLenum srcAlpha = HgiGLConversions::GetBlendFactor(
                colorAttachment.srcAlphaBlendFactor);
            GLenum dstAlpha = HgiGLConversions::GetBlendFactor(
                colorAttachment.dstAlphaBlendFactor);

            GLenum colorOp = HgiGLConversions::GetBlendEquation(
                colorAttachment.colorBlendOp);
            GLenum alphaOp = HgiGLConversions::GetBlendEquation(
                colorAttachment.alphaBlendOp);

            glBlendFuncSeparatei(i, srcColor, dstColor, srcAlpha, dstAlpha);
            glBlendEquationSeparatei(i, colorOp, alphaOp);
        }

        HgiAttachmentDesc const& depthAttachment =
            desc.depthAttachmentDesc;
        if (desc.depthTexture &&
            depthAttachment.loadOp == HgiAttachmentLoadOpClear) {
            glClearBufferfv(GL_DEPTH, 0, depthAttachment.clearValue.data());
        }

        // Setup blending
        if (blendEnabled) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }

        HGIGL_POST_PENDING_GL_ERRORS();
    };
}

PXR_NAMESPACE_CLOSE_SCOPE
