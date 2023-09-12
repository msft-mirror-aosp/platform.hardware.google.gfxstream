# Regenerating Vulkan HPP for Gfxstream

```
mkdir vulkan-hpp
cd vulkan-hpp
git clone --recurse-submodules https://github.com/KhronosGroup/Vulkan-Hpp.git .

# Overwrite the vk.xml with Gfxstream's version
cp hardware/google/gfxstream/codegen/vulkan/vulkan-docs/xml/vk.xml Vulkan-Headers/registry/vk.xml

# In VulkanHppGenerator.cpp's `generateCommandResultSingleSuccessNoErrors()` function, comment
# out the `if ( vectorParams.empty() )` so that the generator can handle
# `vkQueueSignalReleaseImageANDROID()`.

cmake -DVULKAN_HPP_RUN_GENERATOR=ON -B build
cmake --build .

# Copy the generated headers back into hardware/google/gfxstream
cp vulkan/*.hpp hardware/google/gfxstream/codegen/vulkan/vulkan-hpp/generated
```