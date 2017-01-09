/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "GLClientState.h"
#include "GLESTextureUtils.h"
#include "ErrorLog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glUtils.h"
#include <cutils/log.h>

#ifndef MAX
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif

// Don't include these in the .h file, or we get weird compile errors.
#include <GLES3/gl3.h>
#include <GLES3/gl31.h>

void GLClientState::init() {
    m_initialized = false;
    m_nLocations = CODEC_MAX_VERTEX_ATTRIBUTES;

    m_arrayBuffer = 0;
    m_max_vertex_attrib_bindings = m_nLocations;
    addVertexArrayObject(0);
    setVertexArrayObject(0);
    // init gl constans;
    m_currVaoState[VERTEX_LOCATION].glConst = GL_VERTEX_ARRAY;
    m_currVaoState[NORMAL_LOCATION].glConst = GL_NORMAL_ARRAY;
    m_currVaoState[COLOR_LOCATION].glConst = GL_COLOR_ARRAY;
    m_currVaoState[POINTSIZE_LOCATION].glConst = GL_POINT_SIZE_ARRAY_OES;
    m_currVaoState[TEXCOORD0_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[TEXCOORD1_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[TEXCOORD2_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[TEXCOORD3_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[TEXCOORD4_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[TEXCOORD5_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[TEXCOORD6_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[TEXCOORD7_LOCATION].glConst = GL_TEXTURE_COORD_ARRAY;
    m_currVaoState[MATRIXINDEX_LOCATION].glConst = GL_MATRIX_INDEX_ARRAY_OES;
    m_currVaoState[WEIGHT_LOCATION].glConst = GL_WEIGHT_ARRAY_OES;

    m_copyReadBuffer = 0;
    m_copyWriteBuffer = 0;
    m_pixelPackBuffer = 0;
    m_pixelUnpackBuffer = 0;
    m_transformFeedbackBuffer = 0;
    m_uniformBuffer = 0;
    m_atomicCounterBuffer = 0;
    m_dispatchIndirectBuffer = 0;
    m_drawIndirectBuffer = 0;
    m_shaderStorageBuffer = 0;

    m_transformFeedbackActiveUnpaused = false;

    // to be modified later when these are queried from host.
    m_max_transform_feedback_separate_attribs = 0;
    m_max_uniform_buffer_bindings = 0;
    m_max_atomic_counter_buffer_bindings = 0;
    m_max_shader_storage_buffer_bindings = 0;

    m_activeTexture = 0;
    m_currentProgram = 0;

    m_pixelStore.unpack_alignment = 4;
    m_pixelStore.pack_alignment = 4;

    m_pixelStore.unpack_row_length = 0;
    m_pixelStore.unpack_image_height = 0;
    m_pixelStore.unpack_skip_pixels = 0;
    m_pixelStore.unpack_skip_rows = 0;
    m_pixelStore.unpack_skip_images = 0;

    m_pixelStore.pack_row_length = 0;
    m_pixelStore.pack_skip_pixels = 0;
    m_pixelStore.pack_skip_rows = 0;

    memset(m_tex.unit, 0, sizeof(m_tex.unit));
    m_tex.activeUnit = &m_tex.unit[0];
    m_tex.textureRecs = NULL;

    mRboState.boundRenderbuffer = 0;
    mRboState.boundRenderbufferIndex = 0;
    addFreshRenderbuffer(0);

    mFboState.boundFramebuffer = 0;
    mFboState.boundFramebufferIndex = 0;
    mFboState.fboCheckStatus = GL_NONE;
    addFreshFramebuffer(0);

    m_maxVertexAttribsDirty = true;
}

GLClientState::GLClientState()
{
    init();
}

GLClientState::GLClientState(int majorVersion, int minorVersion) :
    m_glesMajorVersion(majorVersion),
    m_glesMinorVersion(minorVersion) {
    init();
}

GLClientState::~GLClientState()
{
}

void GLClientState::enable(int location, int state)
{
    m_currVaoState[location].enableDirty |= (state != m_currVaoState[location].enabled);
    m_currVaoState[location].enabled = state;
}

void GLClientState::setVertexAttribState(int location, int size, GLenum type, GLboolean normalized, GLsizei stride, const void *data, bool isInt)
{
    m_currVaoState[location].size = size;
    m_currVaoState[location].type = type;
    m_currVaoState[location].stride = stride;
    m_currVaoState[location].data = (void*)data;
    m_currVaoState[location].bufferObject = m_arrayBuffer;
    m_currVaoState[location].elementSize = size ? (glSizeof(type) * size) : 0;
    switch (type) {
        case GL_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            m_currVaoState[location].elementSize =
                m_currVaoState[location].elementSize / 4;
            break;
        default:
            break;
    }
    m_currVaoState[location].normalized = normalized;
    m_currVaoState[location].isInt = isInt;
}

void GLClientState::setVertexBindingDivisor(int bindingindex, GLuint divisor) {
    m_currVaoState.bufferBinding(bindingindex).divisor = divisor;
}

const GLClientState::BufferBinding& GLClientState::getCurrAttributeBindingInfo(int attribindex) {
    return m_currVaoState.bufferBindings_const().at(m_currVaoState[attribindex].bindingindex);
}

void GLClientState::setVertexAttribBinding(int attribindex, int bindingindex) {
    m_currVaoState[attribindex].bindingindex = bindingindex;
}

void GLClientState::setVertexAttribFormat(int location, int size, GLenum type, GLboolean normalized, GLuint reloffset, bool isInt) {
    m_currVaoState[location].size = size;
    m_currVaoState[location].type = type;
    m_currVaoState[location].normalized = normalized;
    m_currVaoState[location].reloffset = reloffset;
    m_currVaoState[location].elementSize = size ? (glSizeof(type) * size) : 0;
    switch (type) {
        case GL_INT_2_10_10_10_REV:
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            m_currVaoState[location].elementSize =
                m_currVaoState[location].elementSize / 4;
            break;
        default:
            break;
    }
    m_currVaoState[location].isInt = isInt;
}

void GLClientState::addVertexArrayObjects(GLsizei n, GLuint* arrays) {
    for (GLsizei i = 0; i < n; i++) {
        addVertexArrayObject(arrays[i]);
    }
}

void GLClientState::removeVertexArrayObjects(GLsizei n, const GLuint* arrays) {
    for (GLsizei i = 0; i < n; i++) {
        if (arrays[i] && m_currVaoState.vaoId() == arrays[i]) {
            setVertexArrayObject(0);
        }
        removeVertexArrayObject(arrays[i]);
    }
}

void GLClientState::addVertexArrayObject(GLuint name) {
    if (m_vaoMap.find(name) !=
        m_vaoMap.end()) {
        ALOGE("%s: ERROR: %u already part of current VAO state!",
              __FUNCTION__, name);
        return;
    }

    m_vaoMap.insert(
            VAOStateMap::value_type(
                name,
                VAOState(0, m_nLocations, std::max(m_nLocations, m_max_vertex_attrib_bindings))));
    VertexAttribStateVector& attribState =
        m_vaoMap.at(name).attribState;
    for (int i = 0; i < m_nLocations; i++) {
        attribState[i].enabled = 0;
        attribState[i].enableDirty = false;
        attribState[i].data = 0;
        attribState[i].reloffset = 0;
        attribState[i].bindingindex = i;
        attribState[i].divisor = 0;
        attribState[i].size = 4; // 4 is the default size
        attribState[i].type = GL_FLOAT; // GL_FLOAT is the default type
    }
}

void GLClientState::removeVertexArrayObject(GLuint name) {
    if (name == 0) {
        ALOGE("%s: ERROR: cannot delete VAO 0!",
              __FUNCTION__);
        return;
    }
    if (m_vaoMap.find(name) ==
        m_vaoMap.end()) {
        ALOGE("%s: ERROR: %u not found in VAO state!",
              __FUNCTION__, name);
        return;
    }
    m_vaoMap.erase(name);
}

void GLClientState::setVertexArrayObject(GLuint name) {
    if (m_vaoMap.find(name) ==
        m_vaoMap.end()) {
        ALOGE("%s: ERROR: %u not found in VAO state!",
              __FUNCTION__, name);
        return;
    }

    if (name && m_currVaoState.vaoId() == name) {
        ALOGV("%s: set vao to self, no-op (%u)",
              __FUNCTION__, name);
        return;
    }

    m_currVaoState =
        VAOStateRef(m_vaoMap.find(name));
    ALOGV("%s: set vao to %u (%u) %u %u", __FUNCTION__,
            name,
            m_currVaoState.vaoId(),
            m_arrayBuffer,
            m_currVaoState.iboId());
}

bool GLClientState::isVertexArrayObject(GLuint vao) const {
    return m_vaoMap.find(vao) != m_vaoMap.end();
}

const GLClientState::VertexAttribState& GLClientState::getState(int location)
{
    return m_currVaoState[location];
}

const GLClientState::VertexAttribState& GLClientState::getStateAndEnableDirty(int location, bool *enableChanged)
{
    if (enableChanged) {
        *enableChanged = m_currVaoState[location].enableDirty;
    }

    m_currVaoState[location].enableDirty = false;
    return m_currVaoState[location];
}

int GLClientState::getLocation(GLenum loc)
{
    int retval;

    switch(loc) {
    case GL_VERTEX_ARRAY:
        retval = int(VERTEX_LOCATION);
        break;
    case GL_NORMAL_ARRAY:
        retval = int(NORMAL_LOCATION);
        break;
    case GL_COLOR_ARRAY:
        retval = int(COLOR_LOCATION);
        break;
    case GL_POINT_SIZE_ARRAY_OES:
        retval = int(POINTSIZE_LOCATION);
        break;
    case GL_TEXTURE_COORD_ARRAY:
        retval = int (TEXCOORD0_LOCATION + m_activeTexture);
        break;
    case GL_MATRIX_INDEX_ARRAY_OES:
        retval = int (MATRIXINDEX_LOCATION);
        break;
    case GL_WEIGHT_ARRAY_OES:
        retval = int (WEIGHT_LOCATION);
        break;
    default:
        retval = loc;
    }
    return retval;
}

void GLClientState::unBindBuffer(GLuint id) {
    if (m_arrayBuffer == id) m_arrayBuffer = 0;
    if (m_currVaoState.iboId() == id) m_currVaoState.iboId() = 0;
    if (m_copyReadBuffer == id)
        m_copyReadBuffer = 0;
    if (m_copyWriteBuffer == id)
        m_copyWriteBuffer = 0;
    if (m_pixelPackBuffer == id)
        m_pixelPackBuffer = 0;
    if (m_pixelUnpackBuffer == id)
        m_pixelUnpackBuffer = 0;
    if (m_transformFeedbackBuffer == id)
        m_transformFeedbackBuffer = 0;
    if (m_uniformBuffer == id)
        m_uniformBuffer = 0;
    if (m_atomicCounterBuffer == id)
        m_atomicCounterBuffer = 0;
    if (m_dispatchIndirectBuffer == id)
        m_dispatchIndirectBuffer = 0;
    if (m_drawIndirectBuffer == id)
        m_drawIndirectBuffer = 0;
    if (m_shaderStorageBuffer == id)
        m_shaderStorageBuffer = 0;
}

int GLClientState::bindBuffer(GLenum target, GLuint id)
{
    int err = 0;
    switch(target) {
    case GL_ARRAY_BUFFER:
        m_arrayBuffer = id;
        break;
    case GL_ELEMENT_ARRAY_BUFFER:
        m_currVaoState.iboId() = id;
        break;
    case GL_COPY_READ_BUFFER:
        m_copyReadBuffer = id;
        break;
    case GL_COPY_WRITE_BUFFER:
        m_copyWriteBuffer = id;
        break;
    case GL_PIXEL_PACK_BUFFER:
        m_pixelPackBuffer = id;
        break;
    case GL_PIXEL_UNPACK_BUFFER:
        m_pixelUnpackBuffer = id;
        break;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        m_transformFeedbackBuffer = id;
        break;
    case GL_UNIFORM_BUFFER:
        m_uniformBuffer = id;
        break;
    case GL_ATOMIC_COUNTER_BUFFER:
        m_atomicCounterBuffer = id;
        break;
    case GL_DISPATCH_INDIRECT_BUFFER:
        m_dispatchIndirectBuffer = id;
        break;
    case GL_DRAW_INDIRECT_BUFFER:
        m_drawIndirectBuffer = id;
        break;
    case GL_SHADER_STORAGE_BUFFER:
        m_shaderStorageBuffer = id;
        break;
    default:
        err = -1;
    }
    return err;
}

void GLClientState::bindIndexedBuffer(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size, GLintptr stride, GLintptr effectiveStride) {
    switch (target) {
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        m_indexedTransformFeedbackBuffers[index].buffer = buffer;
        m_indexedTransformFeedbackBuffers[index].offset = offset;
        m_indexedTransformFeedbackBuffers[index].size = size;
        m_indexedTransformFeedbackBuffers[index].stride = stride;
        break;
    case GL_UNIFORM_BUFFER:
        m_indexedUniformBuffers[index].buffer = buffer;
        m_indexedUniformBuffers[index].offset = offset;
        m_indexedUniformBuffers[index].size = size;
        m_indexedUniformBuffers[index].stride = stride;
        break;
    case GL_ATOMIC_COUNTER_BUFFER:
        m_indexedAtomicCounterBuffers[index].buffer = buffer;
        m_indexedAtomicCounterBuffers[index].offset = offset;
        m_indexedAtomicCounterBuffers[index].size = size;
        m_indexedAtomicCounterBuffers[index].stride = stride;
        break;
    case GL_SHADER_STORAGE_BUFFER:
        m_indexedShaderStorageBuffers[index].buffer = buffer;
        m_indexedShaderStorageBuffers[index].offset = offset;
        m_indexedShaderStorageBuffers[index].size = size;
        m_indexedShaderStorageBuffers[index].stride = stride;
        break;
    default:
        m_currVaoState.bufferBinding(index).buffer = buffer;
        m_currVaoState.bufferBinding(index).offset = offset;
        m_currVaoState.bufferBinding(index).size = size;
        m_currVaoState.bufferBinding(index).stride = stride;
        m_currVaoState.bufferBinding(index).effectiveStride = effectiveStride;
        return;
    }
}

int GLClientState::getMaxIndexedBufferBindings(GLenum target) const {
    switch (target) {
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        return m_indexedTransformFeedbackBuffers.size();
    case GL_UNIFORM_BUFFER:
        return m_indexedUniformBuffers.size();
    case GL_ATOMIC_COUNTER_BUFFER:
        return m_indexedAtomicCounterBuffers.size();
    case GL_SHADER_STORAGE_BUFFER:
        return m_indexedShaderStorageBuffers.size();
    default:
        return m_currVaoState.bufferBindings_const().size();
    }
}

int GLClientState::getBuffer(GLenum target) {
    int ret=0;
    switch (target) {
        case GL_ARRAY_BUFFER:
            ret = m_arrayBuffer;
            break;
        case GL_ELEMENT_ARRAY_BUFFER:
            ret = m_currVaoState.iboId();
            break;
        case GL_COPY_READ_BUFFER:
            ret = m_copyReadBuffer;
            break;
        case GL_COPY_WRITE_BUFFER:
            ret = m_copyWriteBuffer;
            break;
        case GL_PIXEL_PACK_BUFFER:
            ret = m_pixelPackBuffer;
            break;
        case GL_PIXEL_UNPACK_BUFFER:
            ret = m_pixelUnpackBuffer;
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER:
            ret = m_transformFeedbackBuffer;
            break;
        case GL_UNIFORM_BUFFER:
            ret = m_uniformBuffer;
            break;
        case GL_ATOMIC_COUNTER_BUFFER:
            ret = m_atomicCounterBuffer;
            break;
        case GL_DISPATCH_INDIRECT_BUFFER:
            ret = m_dispatchIndirectBuffer;
            break;
        case GL_DRAW_INDIRECT_BUFFER:
            ret = m_drawIndirectBuffer;
            break;
        case GL_SHADER_STORAGE_BUFFER:
            ret = m_shaderStorageBuffer;
            break;
        default:
            ret = -1;
    }
    return ret;
}

void GLClientState::getClientStatePointer(GLenum pname, GLvoid** params)
{
    GLenum which_state = -1;
    switch (pname) {
    case GL_VERTEX_ARRAY_POINTER: {
        which_state = GLClientState::VERTEX_LOCATION;
        break;
        }
    case GL_NORMAL_ARRAY_POINTER: {
        which_state = GLClientState::NORMAL_LOCATION;
        break;
        }
    case GL_COLOR_ARRAY_POINTER: {
        which_state = GLClientState::COLOR_LOCATION;
        break;
        }
    case GL_TEXTURE_COORD_ARRAY_POINTER: {
        which_state = getActiveTexture() + GLClientState::TEXCOORD0_LOCATION;
        break;
        }
    case GL_POINT_SIZE_ARRAY_POINTER_OES: {
        which_state = GLClientState::POINTSIZE_LOCATION;
        break;
        }
    case GL_MATRIX_INDEX_ARRAY_POINTER_OES: {
        which_state = GLClientState::MATRIXINDEX_LOCATION;
        break;
        }
    case GL_WEIGHT_ARRAY_POINTER_OES: {
        which_state = GLClientState::WEIGHT_LOCATION;
        break;
        }
    }
    if (which_state != -1)
        *params = getState(which_state).data;
}

int GLClientState::setPixelStore(GLenum param, GLint value)
{
    int retval = 0;
    switch(param) {
    case GL_UNPACK_ALIGNMENT:
        m_pixelStore.unpack_alignment = value;
        break;
    case GL_PACK_ALIGNMENT:
        m_pixelStore.pack_alignment = value;
        break;
    case GL_UNPACK_ROW_LENGTH:
        m_pixelStore.unpack_row_length = value;
        break;
    case GL_UNPACK_IMAGE_HEIGHT:
        m_pixelStore.unpack_image_height = value;
        break;
    case GL_UNPACK_SKIP_PIXELS:
        m_pixelStore.unpack_skip_pixels = value;
        break;
    case GL_UNPACK_SKIP_ROWS:
        m_pixelStore.unpack_skip_rows = value;
        break;
    case GL_UNPACK_SKIP_IMAGES:
        m_pixelStore.unpack_skip_images = value;
        break;
    case GL_PACK_ROW_LENGTH:
        m_pixelStore.pack_row_length = value;
        break;
    case GL_PACK_SKIP_PIXELS:
        m_pixelStore.pack_skip_pixels = value;
        break;
    case GL_PACK_SKIP_ROWS:
        m_pixelStore.pack_skip_rows = value;
        break;
    default:
        retval = GL_INVALID_ENUM;
    }
    return retval;
}


size_t GLClientState::pixelDataSize(GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, int pack) const
{
    if (width <= 0 || height <= 0 || depth <= 0) return 0;

    ALOGV("%s: pack? %d", __FUNCTION__, pack);
    if (pack) {
        ALOGV("%s: pack stats", __FUNCTION__);
        ALOGV("%s: pack align %d", __FUNCTION__, m_pixelStore.pack_alignment);
        ALOGV("%s: pack rowlen %d", __FUNCTION__, m_pixelStore.pack_row_length);
        ALOGV("%s: pack skippixels %d", __FUNCTION__, m_pixelStore.pack_skip_pixels);
        ALOGV("%s: pack skiprows %d", __FUNCTION__, m_pixelStore.pack_skip_rows);
    } else {
        ALOGV("%s: unpack stats", __FUNCTION__);
        ALOGV("%s: unpack align %d", __FUNCTION__, m_pixelStore.unpack_alignment);
        ALOGV("%s: unpack rowlen %d", __FUNCTION__, m_pixelStore.unpack_row_length);
        ALOGV("%s: unpack imgheight %d", __FUNCTION__, m_pixelStore.unpack_image_height);
        ALOGV("%s: unpack skippixels %d", __FUNCTION__, m_pixelStore.unpack_skip_pixels);
        ALOGV("%s: unpack skiprows %d", __FUNCTION__, m_pixelStore.unpack_skip_rows);
        ALOGV("%s: unpack skipimages %d", __FUNCTION__, m_pixelStore.unpack_skip_images);
    }
    return GLESTextureUtils::computeTotalImageSize(
            width, height, depth,
            format, type,
            pack ? m_pixelStore.pack_alignment : m_pixelStore.unpack_alignment,
            pack ? m_pixelStore.pack_row_length : m_pixelStore.unpack_row_length,
            pack ? 0 : m_pixelStore.unpack_image_height,
            pack ? m_pixelStore.pack_skip_pixels : m_pixelStore.unpack_skip_pixels,
            pack ? m_pixelStore.pack_skip_rows : m_pixelStore.unpack_skip_rows,
            pack ? 0 : m_pixelStore.unpack_skip_images);
}

size_t GLClientState::pboNeededDataSize(GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, int pack) const
{
    if (width <= 0 || height <= 0 || depth <= 0) return 0;

    ALOGV("%s: pack? %d", __FUNCTION__, pack);
    if (pack) {
        ALOGV("%s: pack stats", __FUNCTION__);
        ALOGV("%s: pack align %d", __FUNCTION__, m_pixelStore.pack_alignment);
        ALOGV("%s: pack rowlen %d", __FUNCTION__, m_pixelStore.pack_row_length);
        ALOGV("%s: pack skippixels %d", __FUNCTION__, m_pixelStore.pack_skip_pixels);
        ALOGV("%s: pack skiprows %d", __FUNCTION__, m_pixelStore.pack_skip_rows);
    } else {
        ALOGV("%s: unpack stats", __FUNCTION__);
        ALOGV("%s: unpack align %d", __FUNCTION__, m_pixelStore.unpack_alignment);
        ALOGV("%s: unpack rowlen %d", __FUNCTION__, m_pixelStore.unpack_row_length);
        ALOGV("%s: unpack imgheight %d", __FUNCTION__, m_pixelStore.unpack_image_height);
        ALOGV("%s: unpack skippixels %d", __FUNCTION__, m_pixelStore.unpack_skip_pixels);
        ALOGV("%s: unpack skiprows %d", __FUNCTION__, m_pixelStore.unpack_skip_rows);
        ALOGV("%s: unpack skipimages %d", __FUNCTION__, m_pixelStore.unpack_skip_images);
    }
    return GLESTextureUtils::computeNeededBufferSize(
            width, height, depth,
            format, type,
            pack ? m_pixelStore.pack_alignment : m_pixelStore.unpack_alignment,
            pack ? m_pixelStore.pack_row_length : m_pixelStore.unpack_row_length,
            pack ? 0 : m_pixelStore.unpack_image_height,
            pack ? m_pixelStore.pack_skip_pixels : m_pixelStore.unpack_skip_pixels,
            pack ? m_pixelStore.pack_skip_rows : m_pixelStore.unpack_skip_rows,
            pack ? 0 : m_pixelStore.unpack_skip_images);
}


size_t GLClientState::clearBufferNumElts(GLenum buffer) const
{
    switch (buffer) {
    case GL_COLOR:
        return 4;
    case GL_DEPTH:
    case GL_STENCIL:
        return 1;
    }
    return 1;
}

void GLClientState::setNumActiveUniformsInUniformBlock(GLuint program, GLuint uniformBlockIndex, GLint numActiveUniforms) {
    UniformBlockInfoKey key;
    key.program = program;
    key.uniformBlockIndex = uniformBlockIndex;

    UniformBlockUniformInfo info;
    info.numActiveUniforms = (size_t)numActiveUniforms;

    m_uniformBlockInfoMap[key] = info;
}

size_t GLClientState::numActiveUniformsInUniformBlock(GLuint program, GLuint uniformBlockIndex) const {
    UniformBlockInfoKey key;
    key.program = program;
    key.uniformBlockIndex = uniformBlockIndex;
    UniformBlockInfoMap::const_iterator it =
        m_uniformBlockInfoMap.find(key);
    if (it == m_uniformBlockInfoMap.end()) return 0;
    return it->second.numActiveUniforms;
}

}

GLenum GLClientState::setActiveTextureUnit(GLenum texture)
{
    GLuint unit = texture - GL_TEXTURE0;
    if (unit >= MAX_TEXTURE_UNITS) {
        return GL_INVALID_ENUM;
    }
    m_tex.activeUnit = &m_tex.unit[unit];
    return GL_NO_ERROR;
}

GLenum GLClientState::getActiveTextureUnit() const
{
    return GL_TEXTURE0 + (m_tex.activeUnit - &m_tex.unit[0]);
}

void GLClientState::enableTextureTarget(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_2D:
        m_tex.activeUnit->enables |= (1u << TEXTURE_2D);
        break;
    case GL_TEXTURE_EXTERNAL_OES:
        m_tex.activeUnit->enables |= (1u << TEXTURE_EXTERNAL);
        break;
    }
}

void GLClientState::disableTextureTarget(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_2D:
        m_tex.activeUnit->enables &= ~(1u << TEXTURE_2D);
        break;
    case GL_TEXTURE_EXTERNAL_OES:
        m_tex.activeUnit->enables &= ~(1u << TEXTURE_EXTERNAL);
        break;
    }
}

GLenum GLClientState::getPriorityEnabledTarget(GLenum allDisabled) const
{
    unsigned int enables = m_tex.activeUnit->enables;
    if (enables & (1u << TEXTURE_EXTERNAL)) {
        return GL_TEXTURE_EXTERNAL_OES;
    } else if (enables & (1u << TEXTURE_2D)) {
        return GL_TEXTURE_2D;
    } else {
        return allDisabled;
    }
}

int GLClientState::compareTexId(const void* pid, const void* prec)
{
    const GLuint* id = (const GLuint*)pid;
    const TextureRec* rec = (const TextureRec*)prec;
    return (GLint)(*id) - (GLint)rec->id;
}

GLenum GLClientState::bindTexture(GLenum target, GLuint texture,
        GLboolean* firstUse)
{
    assert(m_tex.textureRecs);
    GLboolean first = GL_FALSE;

    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) {
        texrec = addTextureRec(texture, target);
    }

    if (texture && target != texrec->target &&
        (target != GL_TEXTURE_EXTERNAL_OES &&
         texrec->target != GL_TEXTURE_EXTERNAL_OES)) {
        ALOGD("%s: issue GL_INVALID_OPERATION: target 0x%x texrectarget 0x%x texture %u", __FUNCTION__, target, texrec->target, texture);
    }

    switch (target) {
    case GL_TEXTURE_2D:
        m_tex.activeUnit->texture[TEXTURE_2D] = texture;
        break;
    case GL_TEXTURE_EXTERNAL_OES:
        m_tex.activeUnit->texture[TEXTURE_EXTERNAL] = texture;
        break;
    case GL_TEXTURE_CUBE_MAP:
        m_tex.activeUnit->texture[TEXTURE_CUBE_MAP] = texture;
        break;
    case GL_TEXTURE_2D_ARRAY:
        m_tex.activeUnit->texture[TEXTURE_2D_ARRAY] = texture;
        break;
    case GL_TEXTURE_3D:
        m_tex.activeUnit->texture[TEXTURE_3D] = texture;
        break;
    case GL_TEXTURE_2D_MULTISAMPLE:
        m_tex.activeUnit->texture[TEXTURE_2D_MULTISAMPLE] = texture;
        break;
    }

    if (firstUse) {
        *firstUse = first;
    }

    return GL_NO_ERROR;
}

void GLClientState::setBoundEGLImage(GLenum target, GLeglImageOES image) {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) return;
    texrec->boundEGLImage = true;
}

TextureRec* GLClientState::addTextureRec(GLuint id, GLenum target)
{
    TextureRec* tex = new TextureRec;
    tex->id = id;
    tex->target = target;
    tex->format = -1;
    tex->multisamples = 0;
    tex->immutable = false;
    tex->boundEGLImage = false;
    tex->dims = new TextureDims;

    (*(m_tex.textureRecs))[id] = tex;
    return tex;
}

TextureRec* GLClientState::getTextureRec(GLuint id) const {
    SharedTextureDataMap::const_iterator it =
        m_tex.textureRecs->find(id);
    if (it == m_tex.textureRecs->end()) {
        return NULL;
    }
    return it->second;
}

void GLClientState::setBoundTextureInternalFormat(GLenum target, GLint internalformat) {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) return;
    texrec->internalformat = internalformat;
}

void GLClientState::setBoundTextureFormat(GLenum target, GLenum format) {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) return;
    texrec->format = format;
}

void GLClientState::setBoundTextureType(GLenum target, GLenum type) {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) return;
    texrec->type = type;
}

void GLClientState::setBoundTextureDims(GLenum target, GLsizei level, GLsizei width, GLsizei height, GLsizei depth) {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) {
        return;
    }

    if (level == -1) {
        GLsizei curr_width = width;
        GLsizei curr_height = height;
        GLsizei curr_depth = depth;
        GLsizei curr_level = 0;

        while (true) {
            texrec->dims->widths[curr_level] = curr_width;
            texrec->dims->heights[curr_level] = curr_height;
            texrec->dims->depths[curr_level] = curr_depth;
            if (curr_width >> 1 == 0 &&
                curr_height >> 1 == 0 &&
                ((target == GL_TEXTURE_3D && curr_depth == 0) ||
                 true)) {
                break;
            }
            curr_width = (curr_width >> 1) ? (curr_width >> 1) : 1;
            curr_height = (curr_height >> 1) ? (curr_height >> 1) : 1;
            if (target == GL_TEXTURE_3D) {
                curr_depth = (curr_depth >> 1) ? (curr_depth >> 1) : 1;
            }
            curr_level++;
        }

    } else {
        texrec->dims->widths[level] = width;
        texrec->dims->heights[level] = height;
        texrec->dims->depths[level] = depth;
    }
}

void GLClientState::setBoundTextureSamples(GLenum target, GLsizei samples) {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) return;
    texrec->multisamples = samples;
}

void GLClientState::setBoundTextureImmutableFormat(GLenum target) {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) return;
    texrec->immutable = true;
}

bool GLClientState::isBoundTextureImmutableFormat(GLenum target) const {
    GLuint texture = getBoundTexture(target);
    TextureRec* texrec = getTextureRec(texture);
    if (!texrec) return false;
    return texrec->immutable;
}

GLuint GLClientState::getBoundTexture(GLenum target) const
{
    switch (target) {
    case GL_TEXTURE_2D:
        return m_tex.activeUnit->texture[TEXTURE_2D];
    case GL_TEXTURE_EXTERNAL_OES:
        return m_tex.activeUnit->texture[TEXTURE_EXTERNAL];
    case GL_TEXTURE_CUBE_MAP:
        return m_tex.activeUnit->texture[TEXTURE_CUBE_MAP];
    case GL_TEXTURE_2D_ARRAY:
        return m_tex.activeUnit->texture[TEXTURE_2D_ARRAY];
    case GL_TEXTURE_3D:
        return m_tex.activeUnit->texture[TEXTURE_3D];
    case GL_TEXTURE_2D_MULTISAMPLE:
        return m_tex.activeUnit->texture[TEXTURE_2D_MULTISAMPLE];
    default:
        return 0;
    }
}

// BEGIN driver workarounds-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-
// (>' ')><(' '<)(>' ')><(' '<)(>' ')><(' '<)(>' ')><(' '<)(>' ')><(' '<)(>' ')>

static bool unreliableInternalFormat(GLenum internalformat) {
    switch (internalformat) {
    case GL_LUMINANCE:
        return true;
    default:
        return false;
    }
}

void GLClientState::writeCopyTexImageState
    (GLenum target, GLint level, GLenum internalformat) {
    if (unreliableInternalFormat(internalformat)) {
        CubeMapDef entry;
        entry.id = getBoundTexture(GL_TEXTURE_2D);
        entry.target = target;
        entry.level = level;
        entry.internalformat = internalformat;
        m_cubeMapDefs.insert(entry);
    }
}

static GLenum identifyPositiveCubeMapComponent(GLenum target) {
    switch (target) {
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
        return GL_TEXTURE_CUBE_MAP_POSITIVE_X;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
        return GL_TEXTURE_CUBE_MAP_POSITIVE_Y;
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
        return GL_TEXTURE_CUBE_MAP_POSITIVE_Z;
    default:
        return 0;
    }
}

GLenum GLClientState::copyTexImageNeededTarget
    (GLenum target, GLint level, GLenum internalformat) {
    if (unreliableInternalFormat(internalformat)) {
        GLenum positiveComponent =
            identifyPositiveCubeMapComponent(target);
        if (positiveComponent) {
            CubeMapDef query;
            query.id = getBoundTexture(GL_TEXTURE_2D);
            query.target = positiveComponent;
            query.level = level;
            query.internalformat = internalformat;
            if (m_cubeMapDefs.find(query) ==
                m_cubeMapDefs.end()) {
                return positiveComponent;
            }
        }
    }
    return 0;
}

GLenum GLClientState::copyTexImageLuminanceCubeMapAMDWorkaround
    (GLenum target, GLint level, GLenum internalformat) {
    writeCopyTexImageState(target, level, internalformat);
    return copyTexImageNeededTarget(target, level, internalformat);
}

// (>' ')><(' '<)(>' ')><(' '<)(>' ')><(' '<)(>' ')><(' '<)(>' ')><(' '<)(>' ')>
// END driver workarounds-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-~-

void GLClientState::deleteTextures(GLsizei n, const GLuint* textures)
{
    // Updating the textures array could be made more efficient when deleting
    // several textures:
    // - compacting the array could be done in a single pass once the deleted
    //   textures are marked, or
    // - could swap deleted textures to the end and re-sort.
    TextureRec* texrec;
    for (const GLuint* texture = textures; texture != textures + n; texture++) {
        texrec = getTextureRec(*texture);
        if (texrec && texrec->dims) {
            delete texrec->dims;
        }
        if (texrec) {
            m_tex.textureRecs->erase(*texture);
            delete texrec;
            for (TextureUnit* unit = m_tex.unit;
                 unit != m_tex.unit + MAX_TEXTURE_UNITS;
                 unit++)
            {
                if (unit->texture[TEXTURE_2D] == *texture) {
                    unit->texture[TEXTURE_2D] = 0;
                } else if (unit->texture[TEXTURE_EXTERNAL] == *texture) {
                    unit->texture[TEXTURE_EXTERNAL] = 0;
                }
            }
        }
    }
}

// RBO//////////////////////////////////////////////////////////////////////////

void GLClientState::addFreshRenderbuffer(GLuint name) {
    mRboState.rboData.push_back(RboProps());
    RboProps& props = mRboState.rboData.back();
    props.target = GL_RENDERBUFFER;
    props.name = name;
    props.format = GL_NONE;
    props.previouslyBound = false;
}

void GLClientState::addRenderbuffers(GLsizei n, GLuint* renderbuffers) {
    for (size_t i = 0; i < n; i++) {
        addFreshRenderbuffer(renderbuffers[i]);
    }
}

size_t GLClientState::getRboIndex(GLuint name) const {
    for (size_t i = 0; i < mRboState.rboData.size(); i++) {
        if (mRboState.rboData[i].name == name) {
            return i;
        }
    }
    return -1;
}

void GLClientState::removeRenderbuffers(GLsizei n, const GLuint* renderbuffers) {
    size_t bound_rbo_idx = getRboIndex(boundRboProps_const().name);

    std::vector<GLuint> to_remove;
    for (size_t i = 0; i < n; i++) {
        if (renderbuffers[i] != 0) { // Never remove the zero rb.
            to_remove.push_back(getRboIndex(renderbuffers[i]));
        }
    }

    for (size_t i = 0; i < to_remove.size(); i++) {
        mRboState.rboData[to_remove[i]] = mRboState.rboData.back();
        mRboState.rboData.pop_back();
    }

    // If we just deleted the currently bound rb,
    // bind the zero rb
    if (getRboIndex(boundRboProps_const().name) != bound_rbo_idx) {
        bindRenderbuffer(GL_RENDERBUFFER, 0);
    }
}

bool GLClientState::usedRenderbufferName(GLuint name) const {
    for (size_t i = 0; i < mRboState.rboData.size(); i++) {
        if (mRboState.rboData[i].name == name) {
            return true;
        }
    }
    return false;
}

void GLClientState::setBoundRenderbufferIndex() {
    for (size_t i = 0; i < mRboState.rboData.size(); i++) {
        if (mRboState.rboData[i].name == mRboState.boundRenderbuffer) {
            mRboState.boundRenderbufferIndex = i;
            break;
        }
    }
}

RboProps& GLClientState::boundRboProps() {
    return mRboState.rboData[mRboState.boundRenderbufferIndex];
}

const RboProps& GLClientState::boundRboProps_const() const {
    return mRboState.rboData[mRboState.boundRenderbufferIndex];
}

void GLClientState::bindRenderbuffer(GLenum target, GLuint name) {
    // If unused, add it.
    if (!usedRenderbufferName(name)) {
        addFreshRenderbuffer(name);
    }
    mRboState.boundRenderbuffer = name;
    setBoundRenderbufferIndex();
    boundRboProps().target = target;
    boundRboProps().previouslyBound = true;
}

GLuint GLClientState::boundRenderbuffer() const {
    return boundRboProps_const().name;
}

void GLClientState::setBoundRenderbufferFormat(GLenum format) {
    boundRboProps().format = format;
}

// FBO//////////////////////////////////////////////////////////////////////////

// Format querying

GLenum GLClientState::queryRboFormat(GLuint rbo_name) const {
    return mRboState.rboData[getRboIndex(rbo_name)].format;
}

GLint GLClientState::queryTexInternalFormat(GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return -1;
    return texrec->internalformat;
}

GLsizei GLClientState::queryTexWidth(GLsizei level, GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) {
        return 0;
    }
    return texrec->dims->widths[level];
}

GLsizei GLClientState::queryTexHeight(GLsizei level, GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return 0;
    return texrec->dims->heights[level];
}

GLsizei GLClientState::queryTexDepth(GLsizei level, GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return 0;
    return texrec->dims->depths[level];
}

bool GLClientState::queryTexEGLImageBacked(GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return false;
    return texrec->boundEGLImage;
}

GLenum GLClientState::queryTexFormat(GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return -1;
    return texrec->format;
}

GLenum GLClientState::queryTexType(GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return -1;
    return texrec->type;
}

GLsizei GLClientState::queryTexSamples(GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return 0;
    return texrec->multisamples;
}

GLenum GLClientState::queryTexLastBoundTarget(GLuint tex_name) const {
    TextureRec* texrec = getTextureRec(tex_name);
    if (!texrec) return GL_NONE;
    return texrec->target;
}

void GLClientState::getBoundFramebufferFormat(
        GLenum attachment, FboFormatInfo* res_info) const {
    const FboProps& props = boundFboProps_const();

    res_info->type = FBO_ATTACHMENT_NONE;
    res_info->rb_format = GL_NONE;
    res_info->tex_internalformat = -1;
    res_info->tex_format = GL_NONE;
    res_info->tex_type = GL_NONE;

    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        if (props.colorAttachment0_hasRbo) {
            res_info->type = FBO_ATTACHMENT_RENDERBUFFER;
            res_info->rb_format = queryRboFormat(props.colorAttachment0_rbo);
        } else if (props.colorAttachment0_hasTexObj) {
            res_info->type = FBO_ATTACHMENT_TEXTURE;
            res_info->tex_internalformat = queryTexInternalFormat(props.colorAttachment0_texture);
            res_info->tex_format = queryTexFormat(props.colorAttachment0_texture);
            res_info->tex_type = queryTexType(props.colorAttachment0_texture);
        } else {
            res_info->type = FBO_ATTACHMENT_NONE;
        }
        break;
    case GL_DEPTH_ATTACHMENT:
        if (props.depthAttachment_hasRbo) {
            res_info->type = FBO_ATTACHMENT_RENDERBUFFER;
            res_info->rb_format = queryRboFormat(props.depthAttachment_rbo);
        } else if (props.depthAttachment_hasTexObj) {
            res_info->type = FBO_ATTACHMENT_TEXTURE;
            res_info->tex_internalformat = queryTexInternalFormat(props.depthAttachment_texture);
            res_info->tex_format = queryTexFormat(props.depthAttachment_texture);
            res_info->tex_type = queryTexType(props.depthAttachment_texture);
        } else {
            res_info->type = FBO_ATTACHMENT_NONE;
        }
        break;
    case GL_STENCIL_ATTACHMENT:
        if (props.stencilAttachment_hasRbo) {
            res_info->type = FBO_ATTACHMENT_RENDERBUFFER;
            res_info->rb_format = queryRboFormat(props.stencilAttachment_rbo);
        } else if (props.stencilAttachment_hasTexObj) {
            res_info->type = FBO_ATTACHMENT_TEXTURE;
            res_info->tex_internalformat = queryTexInternalFormat(props.stencilAttachment_texture);
            res_info->tex_format = queryTexFormat(props.stencilAttachment_texture);
            res_info->tex_type = queryTexType(props.stencilAttachment_texture);
        } else {
            res_info->type = FBO_ATTACHMENT_NONE;
        }
        break;
    default:
        res_info->type = FBO_ATTACHMENT_NONE;
        break;
    }
}

void GLClientState::addFreshFramebuffer(GLuint name) {
    mFboState.fboData.push_back(FboProps());
    FboProps& props = mFboState.fboData.back();
    props.target = GL_FRAMEBUFFER;
    props.name = name;
    props.previouslyBound = false;

    props.colorAttachment0_texture = 0;
    props.depthAttachment_texture = 0;
    props.stencilAttachment_texture = 0;

    props.colorAttachment0_hasTexObj = false;
    props.depthAttachment_hasTexObj = false;
    props.stencilAttachment_hasTexObj = false;

    props.colorAttachment0_rbo = 0;
    props.depthAttachment_rbo = 0;
    props.stencilAttachment_rbo = 0;

    props.colorAttachment0_hasRbo = false;
    props.depthAttachment_hasRbo = false;
    props.stencilAttachment_hasRbo = false;
}

void GLClientState::addFramebuffers(GLsizei n, GLuint* framebuffers) {
    for (size_t i = 0; i < n; i++) {
        addFreshFramebuffer(framebuffers[i]);
    }
}

size_t GLClientState::getFboIndex(GLuint name) const {
    for (size_t i = 0; i < mFboState.fboData.size(); i++) {
        if (mFboState.fboData[i].name == name) {
            return i;
        }
    }
    return -1;
}


void GLClientState::removeFramebuffers(GLsizei n, const GLuint* framebuffers) {
    size_t bound_fbo_idx = getFboIndex(boundFboProps_const().name);

    std::vector<GLuint> to_remove;
    for (size_t i = 0; i < n; i++) {
        if (framebuffers[i] != 0) { // Never remove the zero fb.
            to_remove.push_back(getFboIndex(framebuffers[i]));
        }
    }

    for (size_t i = 0; i < to_remove.size(); i++) {
        mFboState.fboData[to_remove[i]] = mFboState.fboData.back();
        mFboState.fboData.pop_back();
    }

    // If we just deleted the currently bound fb<
    // bind the zero fb
    if (getFboIndex(boundFboProps_const().name) != bound_fbo_idx) {
        bindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

bool GLClientState::usedFramebufferName(GLuint name) const {
    for (size_t i = 0; i < mFboState.fboData.size(); i++) {
        if (mFboState.fboData[i].name == name) {
            return true;
        }
    }
    return false;
}

void GLClientState::setBoundFramebufferIndex() {
    for (size_t i = 0; i < mFboState.fboData.size(); i++) {
        if (mFboState.fboData[i].name == mFboState.boundFramebuffer) {
            mFboState.boundFramebufferIndex = i;
            break;
        }
    }
}

FboProps& GLClientState::boundFboProps() {
    return mFboState.fboData[mFboState.boundFramebufferIndex];
}

const FboProps& GLClientState::boundFboProps_const() const {
    return mFboState.fboData[mFboState.boundFramebufferIndex];
}

void GLClientState::bindFramebuffer(GLenum target, GLuint name) {
    // If unused, add it.
    if (!usedFramebufferName(name)) {
        addFreshFramebuffer(name);
    }
    mFboState.boundFramebuffer = name;
    setBoundFramebufferIndex();
    boundFboProps().target = target;
    boundFboProps().previouslyBound = true;
}

void GLClientState::setCheckFramebufferStatus(GLenum status) {
    mFboState.fboCheckStatus = status;
}

GLenum GLClientState::getCheckFramebufferStatus() const {
    return mFboState.fboCheckStatus;
}

GLuint GLClientState::boundFramebuffer() const {
    return boundFboProps_const().name;
}

// Texture objects for FBOs/////////////////////////////////////////////////////

void GLClientState::attachTextureObject(GLenum attachment, GLuint texture) {
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        boundFboProps().colorAttachment0_texture = texture;
        boundFboProps().colorAttachment0_hasTexObj = true;
        break;
    case GL_DEPTH_ATTACHMENT:
        boundFboProps().depthAttachment_texture = texture;
        boundFboProps().depthAttachment_hasTexObj = true;
        break;
    case GL_STENCIL_ATTACHMENT:
        boundFboProps().stencilAttachment_texture = texture;
        boundFboProps().stencilAttachment_hasTexObj = true;
        break;
    default:
        break;
    }
}

GLuint GLClientState::getFboAttachmentTextureId(GLenum attachment) const {
    GLuint res;
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        res = boundFboProps_const().colorAttachment0_texture;
        break;
    case GL_DEPTH_ATTACHMENT:
        res = boundFboProps_const().depthAttachment_texture;
        break;
    case GL_STENCIL_ATTACHMENT:
        res = boundFboProps_const().stencilAttachment_texture;
        break;
    default:
        res = 0; // conservative validation for now
    }
    return res;
}

// RBOs for FBOs////////////////////////////////////////////////////////////////

void GLClientState::attachRbo(GLenum attachment, GLuint renderbuffer) {
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        boundFboProps().colorAttachment0_rbo = renderbuffer;
        boundFboProps().colorAttachment0_hasRbo = true;
        break;
    case GL_DEPTH_ATTACHMENT:
        boundFboProps().depthAttachment_rbo = renderbuffer;
        boundFboProps().depthAttachment_hasRbo = true;
        break;
    case GL_STENCIL_ATTACHMENT:
        boundFboProps().stencilAttachment_rbo = renderbuffer;
        boundFboProps().stencilAttachment_hasRbo = true;
        break;
    default:
        break;
    }
}

GLuint GLClientState::getFboAttachmentRboId(GLenum attachment) const {
    GLuint res;
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        res = boundFboProps_const().colorAttachment0_rbo;
        break;
    case GL_DEPTH_ATTACHMENT:
        res = boundFboProps_const().depthAttachment_rbo;
        break;
    case GL_STENCIL_ATTACHMENT:
        res = boundFboProps_const().stencilAttachment_rbo;
        break;
    default:
        res = 0; // conservative validation for now
    }
    return res;
}

bool GLClientState::attachmentHasObject(GLenum attachment) const {
    bool res;
    switch (attachment) {
    case GL_COLOR_ATTACHMENT0:
        res = (boundFboProps_const().colorAttachment0_hasTexObj) ||
              (boundFboProps_const().colorAttachment0_hasRbo);
        break;
    case GL_DEPTH_ATTACHMENT:
        res = (boundFboProps_const().depthAttachment_hasTexObj) ||
              (boundFboProps_const().depthAttachment_hasRbo);
        break;
    case GL_STENCIL_ATTACHMENT:
        res = (boundFboProps_const().stencilAttachment_hasTexObj) ||
              (boundFboProps_const().stencilAttachment_hasRbo);
        break;
    default:
        res = true; // liberal validation for now
    }
    return res;
}

void GLClientState::fromMakeCurrent() {
    FboProps& default_fb_props = mFboState.fboData[getFboIndex(0)];
    default_fb_props.colorAttachment0_hasRbo = true;
    default_fb_props.depthAttachment_hasRbo = true;
    default_fb_props.stencilAttachment_hasRbo = true;
}

void GLClientState::initFromCaps(
    int max_transform_feedback_separate_attribs,
    int max_uniform_buffer_bindings,
    int max_atomic_counter_buffer_bindings,
    int max_shader_storage_buffer_bindings,
    int max_vertex_attrib_bindings,
    int max_color_attachments,
    int max_draw_buffers) {

    m_max_vertex_attrib_bindings = max_vertex_attrib_bindings;

    if (m_glesMajorVersion >= 3) {
        m_max_transform_feedback_separate_attribs = max_transform_feedback_separate_attribs;
        m_max_uniform_buffer_bindings = max_uniform_buffer_bindings;
        m_max_atomic_counter_buffer_bindings = max_atomic_counter_buffer_bindings;
        m_max_shader_storage_buffer_bindings = max_shader_storage_buffer_bindings;

        if (m_max_transform_feedback_separate_attribs)
            m_indexedTransformFeedbackBuffers.resize(m_max_transform_feedback_separate_attribs);
        if (m_max_uniform_buffer_bindings)
            m_indexedUniformBuffers.resize(m_max_uniform_buffer_bindings);
        if (m_max_atomic_counter_buffer_bindings)
            m_indexedAtomicCounterBuffers.resize(m_max_atomic_counter_buffer_bindings);
        if (m_max_shader_storage_buffer_bindings)
            m_indexedShaderStorageBuffers.resize(m_max_shader_storage_buffer_bindings);

        BufferBinding buf0Binding;
        buf0Binding.buffer = 0;
        buf0Binding.offset = 0;
        buf0Binding.size = 0;
        buf0Binding.stride = 0;
        buf0Binding.effectiveStride = 0;

        for (size_t i = 0; i < m_indexedTransformFeedbackBuffers.size(); ++i)
            m_indexedTransformFeedbackBuffers[i] = buf0Binding;
        for (size_t i = 0; i < m_indexedUniformBuffers.size(); ++i)
            m_indexedUniformBuffers[i] = buf0Binding;
        for (size_t i = 0; i < m_indexedAtomicCounterBuffers.size(); ++i)
            m_indexedAtomicCounterBuffers[i] = buf0Binding;
        for (size_t i = 0; i < m_indexedShaderStorageBuffers.size(); ++i)
            m_indexedShaderStorageBuffers[i] = buf0Binding;
    }

    m_max_color_attachments = max_color_attachments;
    m_max_draw_buffers = max_draw_buffers;

    addFreshRenderbuffer(0);
    addFreshFramebuffer(0);

    m_initialized = true;
}

bool GLClientState::needsInitFromCaps() const {
    return !m_initialized;
}
