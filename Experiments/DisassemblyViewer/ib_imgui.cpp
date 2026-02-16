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

#define DONT_INCLUDE_CIMGUI
#include "ib_imgui.h"
#include "iceberg/ib_core.h"
#include <imgui/imgui.h>
#include <sokol/sokol_app.h>
#include <backends/imgui_impl_vulkan.h>
#include <backends/imgui_impl_win32.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static WNDPROC PreviousWindowHandler;
static LRESULT CALLBACK win32ProcShim(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	LRESULT procResult = ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
	if ((procResult == FALSE) && PreviousWindowHandler != NULL)
	{
		procResult = CallWindowProc(PreviousWindowHandler, hWnd, msg, wParam, lParam);
	}

	return procResult;
}

static void vkCheck(VkResult result)
{
	ib_vkCheck(result);
}

void imgui_init(ib_Core const* core, VkFormat outputFormat)
{
	ImGui::CreateContext();

	// Casting away const is a sin. But it must be done for the good of the people.
	void* windowHandle = (void*)sapp_win32_get_hwnd();
    ImGui_ImplWin32_Init(windowHandle);

	ImGuiIO& io = ImGui::GetIO();
    // No multiviewport support for now.
    io.BackendFlags &= ~ImGuiBackendFlags_PlatformHasViewports;

    ImGui_ImplVulkan_InitInfo initInfo = {0};
    initInfo.Instance = core->Instance;
    initInfo.PhysicalDevice = core->PhysicalDevice;
    initInfo.Device = core->LogicalDevice;
    initInfo.QueueFamily = core->Queues[ib_Queue_Graphics].Index;
    initInfo.Queue = core->Queues[ib_Queue_Graphics].Queue;
    initInfo.PipelineCache = core->PipelineCache;
    initInfo.DescriptorPool = core->Descriptors.Pool;
    initInfo.Allocator = ib_NoVkAllocator;
    initInfo.MinImageCount = ib_FramebufferCount;
    initInfo.ImageCount = ib_FramebufferCount;
    initInfo.CheckVkResultFn = &vkCheck;
	initInfo.UseDynamicRendering = true;

	VkFormat attachmentFormat = outputFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = { 0 };
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &attachmentFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    ImGui_ImplVulkan_Init(&initInfo);

	// We need to shim ourselves between sokol and imgui to add a call to ImGui_ImplWin32_WndProcHandler
	// Just override the proc to pass to imgui before we pass to sokol.
	PreviousWindowHandler = (WNDPROC) SetWindowLongPtr((HWND)windowHandle, GWLP_WNDPROC, (LONG_PTR)&win32ProcShim);
}

void imgui_kill(void)
{
	ImGui_ImplWin32_Shutdown();
	ImGui_ImplVulkan_Shutdown();
	ImGui::DestroyContext();
}

void imgui_beginFrame(void)
{
	ImGui_ImplWin32_NewFrame();
	ImGui_ImplVulkan_NewFrame();
	ImGui::NewFrame();
}

void imgui_render(VkCommandBuffer cmd, VkExtent2D outputExtent, ib_Texture* output)
{
	ImGui::Render();

	VkRect2D renderArea = {};
	renderArea.extent = outputExtent;

	VkRenderingAttachmentInfo forwardImageAttachment = {};
	forwardImageAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	forwardImageAttachment.imageView = output->View;
	forwardImageAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	forwardImageAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	forwardImageAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

	VkRenderingInfo renderInfo = {};
	renderInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	renderInfo.renderArea = renderArea;
	renderInfo.layerCount = 1;
	renderInfo.colorAttachmentCount = 1;
	renderInfo.pColorAttachments = &forwardImageAttachment;

	vkCmdBeginRendering(cmd, &renderInfo);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	vkCmdEndRendering(cmd);
}

void imgui_endFrame(void)
{
	ImGui::EndFrame();
}