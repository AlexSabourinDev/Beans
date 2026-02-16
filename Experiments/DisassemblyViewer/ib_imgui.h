// Copyright (C) 2024 Snowed In Studios
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef IB_IMGUI_H
#define IB_IMGUI_H

#include "iceberg/ib_core.h"

// Somewhat annoyingly, we can't include both cimgui.h and imgui.h
// We want imgui.h in ib_imgui.cpp
// So we're allowing it to be disabled here.
#ifndef DONT_INCLUDE_CIMGUI

#ifdef _MSC_VER
        #pragma warning(push)
        #pragma warning(disable:4201)   /* nonstandard extension used: nameless struct/union */
#endif // _MSC_VER

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#ifdef _MSC_VER
        #pragma warning(pop)
#endif // _MSC_VER

#endif // DONT_INCLUDE_CIMGUI

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus

// Need a bridge for the backends that cimgui doesn't cover.
void imgui_init(ib_Core const* core, VkFormat outputFormat);
void imgui_kill(void);
void imgui_beginFrame(void);
void imgui_endFrame(void);
void imgui_render(VkCommandBuffer cmd, VkExtent2D outputExtent, ib_Texture* output);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // IB_IMGUI_H