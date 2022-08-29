# Copyright (c) 2022 The Android Open Source Project
# Copyright (c) 2022 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from .wrapperdefs import VulkanWrapperGenerator


class VulkanGfxstreamStructureType(VulkanWrapperGenerator):
    def __init__(self, module, typeInfo):
        super().__init__(module, typeInfo)

    def onGenGroup(self, groupinfo, groupName, alias=None):
        super().onGenGroup(groupinfo, groupName, alias)
        elem = groupinfo.elem
        if (not elem.get('type') == 'enum'):
            return
        if (not elem.get('name') == 'VkStructureType'):
            return
        for enum in elem.findall("enum[@extname='VK_GOOGLE_gfxstream']"):
            name = enum.get('name')
            offset = enum.get('offset')
            self.module.appendHeader(
                f"#define {name} VK_GOOGLE_GFXSTREAM_ENUM(VkStructureType, {offset})\n")
