<!-- markdownlint-disable MD041 -->
<p align="left"><img src="https://vulkan.lunarg.com/img/NewLunarGLogoBlack.png" alt="LunarG" width=263 height=113 /></p>

Copyright &copy; 2015-2020 LunarG, Inc.

[![Creative Commons][3]][4]

[3]: https://i.creativecommons.org/l/by-nd/4.0/88x31.png "Creative Commons License"
[4]: https://creativecommons.org/licenses/by-nd/4.0/

# Vulkan Layer Factory

## Overview ：总览

The Vulkan Layer Factory is a framework based on the canonical Vulkan layer model that
facilitates the implementation of Vulkan Layers. The layer factory hides the majority of the
loader-layer interface, layer boilerplate, setup and initialization, and complexities
of layer development.

翻译：
Vulkan Layer Factory是一个基于标准Vulkan Layer Model的框架，用来促进Vulkan Layer的开发，它隐藏了开发Vulkan Layer所需要的复杂工作，比如：加载器接口、设置、初始化等等。

A complete layer with the attendant support files can be produced by simply creating a
subdirectory in the layer\_factory directory and adding in a simple header file
and then running cmake. This layer can be used just as any other Vulkan layer.

翻译：
创建一个自定义的Vulkan Layer只需要在layer_factory目录下面创建一个子目录，并且添加一个头文件，执行cmake命令，就可以生成。而且，这个Vulkan Layer可以像标准Vulkan Layer一样使用。

The Vulkan Layer Factory framework produces 'Factory Layers' comprising one or more
'interceptor' objects. Interceptor objects override functions to be called before (PreCallApiName)
or after (PostCallApiName) each Vulkan entrypoint of interest. Each interceptor is independent
of all others within a Factory Layer, and their call order is not guaranteed.

### Layer Factory sample code ：示例代码

The base installation of the layer factory contains some sample layers, including
the Demo layer and the Starter Layer. The Starter Layer in particular is meant to serve as
an example of a very simple layer implementation.

该项目包含两个示例，Demo和Starter。


### Create a Factory Layer ：创建Layer


Step 1: 在目录layer_factory创建一个子文件夹，取名luopan

    生成的二进制文件：
    VkLayer_luopan.dll----windows平台
    VkLayer_luopan.so-----linux/Android平台
    对应层的名字为：
    VK_LAYER_LUNARG_luopan

Step 2: 在luopan文件夹中，创建luopan.h头文件，也可以创建对应的luopan.cpp文件，或者更多其他的文件


Step 3: 创建或者拷贝 interceptor_objects.h文件到luopan文件夹中

    在 interceptor_objects.h 文件中添加如下内容：

    #include "luopan.h"

Step 4: 执行cmake命令，编译.

    编译生成so二进制文件和对应json文件，json文件配置了入口functions,一般是：
    vkGetInstanceProcAddr
    vkGetDeviceProcAddr

## Using Layers ：使用Layers

1. 编译VK加载器
2. 设置VK_LAYER_PATH指定Layer的目录

    'set VK_LAYER_PATH=my_layer_path'

3. 使用环境变量激活层.

    `export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation:VK_LAYER_LUNARG_luopan`


### Layer Factory Features

The layer factory provides helper functions for layer authors to simplify layer tasks. These include some
simpler output functions, debug facilities, and simple global intercept functions.


Output Helpers:

Interceptors can use base-class defined output helpers for simpler access to Debug Report Extension output.
These include Information(), Warning(), Performance\_Warning(), and Error(), corresponding to the
VkDebugReportFlagBitsEXT enumerations. Alternatively, the standard layer-provided log\_msg() call can be used
directly, as can printf for standard-out or OutputDebugString for Windows.

Debug Helpers:

A BreakPoint() helper can be used in an intercepted function which will generate a break in a Windows or Linux
debugger.

Global Intercept Helpers:

There are two global intercept helpers, PreCallApiFunction() and PostCallApiFunction(). Overriding these virtual
functions in your intercepter will result in them being called for EVERY API call.

### Details

By creating a child framework object, the factory will generate a full layer and call any overridden functions
in your interceptor.

Here is a simple, and complete, interceptor (the starter\_layer). This layer intercepts the memory allocate and free
functions, tracking the number and total size of device memory allocations. The QueuePresent() function is also intercepted, and
results are outputted on every 60th frame.  Note that the function signatures are identical to those in the specification.

In this example, there is a single interceptor in which the child object is named 'MemAllocLevel' and is instantiated as
'high\_water\_mark'. An layer can contain many interceptors as long as the instantiated object names are unique within that layer.


    #pragma once
    #include <sstream>
    #include <unordered_map>

    static uint32_t display_rate = 60;

    class MemAllocLevel : public layer_factory {
        public:
            // Constructor for interceptor
            MemAllocLevel() : layer_factory(this), number_mem_objects_(0), total_memory_(0), present_count_(0) {};

            // Intercept memory allocation calls and increment counter
            VkResult PostCallAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
                number_mem_objects_++;
                total_memory_ += pAllocateInfo->allocationSize;
                mem_size_map_[*pMemory] = pAllocateInfo->allocationSize;
                return VK_SUCCESS;
            };

            // Intercept free memory calls and update totals
            void PreCallFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator) {
                if (memory != VK_NULL_HANDLE) {
                    number_mem_objects_--;
                    VkDeviceSize this_alloc = mem_size_map_[memory];
                    total_memory_ -= this_alloc;
                }
            }

            VkResult PreCallQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
                present_count_++;
                if (present_count_ >= display_rate) {
                    present_count_ = 0;
                    std::stringstream message;
                    message << "Memory Allocation Count: " << number_mem_objects_ << "\n";
                    message << "Total Memory Allocation Size: " << total_memory_ << "\n\n";
                    Information(message.str());
                }
                return VK_SUCCESS;
            }

        private:
            // Counter for the number of currently active memory allocations
            uint32_t number_mem_objects_;
            VkDeviceSize total_memory_;
            uint32_t present_count_;
            std::unordered_map<VkDeviceMemory, VkDeviceSize> mem_size_map_;
    };

    MemAllocLevel memory_allocation_stats;

### Current known issues

 * CMake MUST be run to pick up and interpret new or deleted factory layers.


