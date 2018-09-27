/*
 * Copyright (C) 2018 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "auto_goldfish_dma_context.h"

namespace {
goldfish_dma_context empty() {
    goldfish_dma_context ctx = {};
    return ctx;
}

void destroy(goldfish_dma_context *ctx) {
    goldfish_dma_unmap(ctx);
    goldfish_dma_free(ctx);
}
}  // namespace

AutoGoldfishDmaContext::AutoGoldfishDmaContext() : m_ctx(empty()) {}

AutoGoldfishDmaContext::AutoGoldfishDmaContext(goldfish_dma_context *ctx)
    : m_ctx(*ctx) {
    *ctx = empty();
}

AutoGoldfishDmaContext::~AutoGoldfishDmaContext() {
    destroy(&m_ctx);
}

void AutoGoldfishDmaContext::reset(goldfish_dma_context *ctx) {
    destroy(&m_ctx);
    m_ctx = *ctx;
    *ctx = empty();
}

goldfish_dma_context AutoGoldfishDmaContext::release() {
    goldfish_dma_context copy = m_ctx;
    m_ctx = empty();
    return copy;
}

