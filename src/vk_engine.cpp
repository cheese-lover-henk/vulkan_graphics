#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include "vk_engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_types.h>

#include "VkBootstrap.h"

#include <chrono>
#include <thread>

#include "vk_images.h"

#include <glm/ext/scalar_constants.hpp>

VulkanEngine* loadedEngine = nullptr;

bool bUseValidationLayers = true;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }
void VulkanEngine::init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fmt::print("SDL_Init Error: {}\n", SDL_GetError());
        exit(1);
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);
    _windowExtent = { 1280, 720 };
    
    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags);
    
    if (_window == nullptr) {
        fmt::print("SDL_CreateWindow Error: {}\n", SDL_GetError());
        exit(1);
    }
    
    init_vulkan();
    init_swapchain();
    init_commands();
    init_sync_structures();

    // everything went fine
    _isInitialized = true;
    
    SDL_ShowWindow(_window);
}

void VulkanEngine::init_vulkan() {
    vkb::InstanceBuilder builder;
    
    auto inst_ret = builder.set_app_name("Vulkan Shit")
        .request_validation_layers(bUseValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 3, 0)
        .build();
    
    vkb::Instance vkb_inst = inst_ret.value();
    _instance = vkb_inst.instance;
    _debug_messenger = vkb_inst.debug_messenger;
    
    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);
    
    VkPhysicalDeviceVulkan13Features features13{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES  };
    features13.dynamicRendering = true;
    features13.synchronization2 = true;
    
    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES  };
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;
    
    vkb::PhysicalDeviceSelector selector { vkb_inst };
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features13)
        .set_required_features_12(features12)
        .set_surface(_surface)
        .select()
        .value();
    
    vkb::DeviceBuilder deviceBuilder{ physicalDevice };
    vkb::Device vkbDevice = deviceBuilder.build().value();
    
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;
    
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
    
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = _chosenGPU;
    allocatorInfo.device = _device;
    allocatorInfo.instance = _instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &_allocator);
    
    _mainDeletionQueue.push_function([&]() {
        vmaDestroyAllocator(_allocator);
    });
}

void VulkanEngine::init_swapchain() {
    create_swapchain(_windowExtent.width, _windowExtent.height);
    
    VkExtent3D drawImageExtent = {
        _windowExtent.width,
        _windowExtent.height,
        1
    };
    
    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;
    
    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    
    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);
    
    VmaAllocationCreateInfo rimg_allocinfo = {};
    rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr);
    
    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));
    
    _mainDeletionQueue.push_function([=]() {
        vkDestroyImageView(_device, _drawImage.imageView, nullptr);
        vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);
    });
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU, _device, _surface };
    
    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_SRGB;
    vkb::Swapchain vkbSwapchain = swapchainBuilder
        .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();
    
    _swapchainExtent = vkbSwapchain.extent;
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
    
    swapchainImageCount = static_cast<uint32_t>(_swapchainImages.size());
}

void VulkanEngine::init_commands() {
    
    // create command pool for commands submitted to the graphics queue
    // able to reset individual command buffers
    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    
    for(int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));
        
        // allocate default command buffer for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);
        
        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }
}

void VulkanEngine::init_sync_structures() {
    // one (1) fence to control when the gpu finished rendering the frame,
    // two (2) semaphores to sync rendering with swapchain
    // fence will start with state signalled, so we can wait on it for the first frame.
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
    
    for(int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));
    }
    
    _renderSemaphores.resize(swapchainImageCount);
    _swapchainSemaphores.resize(swapchainImageCount);
    
    for(int i = 0; i < swapchainImageCount; i++) {
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_swapchainSemaphores[i]));
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_renderSemaphores[i]));
    }
}

void VulkanEngine::destroy_swapchain() {
    vkDestroySwapchainKHR( _device, _swapchain, nullptr);
    
    for(int i = 0; i < _swapchainImageViews.size(); i++) {
        vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    }
}

void VulkanEngine::cleanup() {
    if (_isInitialized) {
        
        vkDeviceWaitIdle(_device);
        
        for(int i = 0; i < FRAME_OVERLAP; i++) {
            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr);
            
            vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
            
            _frames[i]._deletionQueue.flush();
        }
        
        for(int i = 0; i < swapchainImageCount; i++) {
            vkDestroySemaphore(_device, _swapchainSemaphores[i], nullptr);
            vkDestroySemaphore(_device, _renderSemaphores[i], nullptr);
        }
        
        _mainDeletionQueue.flush();
        
        destroy_swapchain();
        
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);
        
        vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
        vkDestroyInstance(_instance, nullptr);
        
        SDL_DestroyWindow(_window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}

float get_y_on_line_from_points(float x1, float y1, float x2, float y2, float x) {
    float a = (y2 - y1) / (x2 - x1);
    float b = -((a * x1) - y1) ;
    
    return (a * x) + b;
}

void VulkanEngine::draw_background(VkCommandBuffer cmd, int swapchainImageIndex) {
    
    // inefficiently written rgb cycle that doesnt even look that good
    VkClearColorValue clearValue;
    float r, g, b;
    int cycle_length = 200;
    float x = _frameNumber % cycle_length;
    //r
    if(x < (cycle_length * (1.0f / 6.0f))) {
        r = 1;
    } else if(x < (cycle_length * (1.0f / 3.0f))) {
        r = get_y_on_line_from_points((cycle_length * (1.0f / 6.0f)), 1.0f, (cycle_length * (1.0f / 3.0f)), 0.0f, x);
    } else if(x < (cycle_length * (2.0f / 3.0f))) {
        r = 0;
    } else if(x < (cycle_length * (5.0f / 6.0f))) {
        r = get_y_on_line_from_points((cycle_length * (2.0f / 3.0f)), 0.0f, (cycle_length * (5.0f / 6.0f)), 1.0f, x);
    } else {
        r = 1;
    }
    //g
    if(x < (cycle_length * (1.0f / 6.0f))) {
        g = get_y_on_line_from_points(0.0f, 0.0f, (cycle_length * (1.0f / 6.0f)), 1.0f, x);
    } else if(x < (cycle_length * 0.5f)) {
        g = 1;
    } else if(x < (cycle_length * (2.0f / 3.0f))) {
        g = get_y_on_line_from_points((cycle_length * 0.5f), 1.0f, (cycle_length * (2.0f / 3.0f)), 0.0f, x);
    } else {
        g = 0;
    }
    //b
    if(x < (cycle_length * (1.0f / 3.0f))) {
        b = 0;
    } else if(x < (cycle_length * 0.5f)) {
        b = get_y_on_line_from_points((cycle_length * (1.0f / 3.0f)), 0.0f, (cycle_length * 0.5f), 1.0f, x);
    } else if(x < (cycle_length * (5.0f / 6.0f))) {
        b = 1;
    } else {
        b = get_y_on_line_from_points((cycle_length * (5.0f / 6.0f)), 1.0f, cycle_length, 0.0f, x);
    }
    
    clearValue = { {r, g, b, 1.f} };
    
    VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
    
    vkCmdClearColorImage(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void VulkanEngine::draw() {
    // wait till gpu finished rendering last frame
    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));
    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));
    
    get_current_frame()._deletionQueue.flush();
    
    // request image index from swapchain
    uint32_t swapchainImageIndex;
    uint32_t semaphoreIndex = _frameNumber % swapchainImageCount;
    VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, _swapchainSemaphores[semaphoreIndex], nullptr, &swapchainImageIndex));
    uint32_t renderSemaphoreIndex = swapchainImageIndex;
    
    // reset command buffer of current frame
    VkCommandBuffer cmd = get_current_frame()._mainCommandBuffer;
    VK_CHECK(vkResetCommandBuffer(cmd, 0));
    
    // begin command buffer recording
    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
    
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    
    draw_background(cmd, swapchainImageIndex);
    
    vkutil::transition_image(cmd, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    
    // end command buffer: we can no longer submit commands, and it is now ready to be executed.
    VK_CHECK(vkEndCommandBuffer(cmd));
    
    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
    
    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, _swapchainSemaphores[semaphoreIndex]);
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, _renderSemaphores[renderSemaphoreIndex]);
    
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);
    
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));
    
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;
    
    presentInfo.pWaitSemaphores = &_renderSemaphores[renderSemaphoreIndex];
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pImageIndices = &swapchainImageIndex;
    
    VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));
    
    _frameNumber++;
}

void VulkanEngine::run() {
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT) {
                bQuit = true;
            }

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        fmt::println("draw");
        draw();
    }
}