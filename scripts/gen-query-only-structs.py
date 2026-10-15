"""Generate goldfish_vk_query_only_structs.h for the gfxstream host.

The table lists every struct that extends VkPhysicalDeviceFeatures2 or VkPhysicalDeviceProperties2,
is plain data (scalars, fixed arrays of scalars, nested plain structs), and has no case in the
host's generated reservedunmarshal_extension_struct() switch. For each one it records the C
layout (offset/size per wire field, in wire order) so the host can decode, re-encode and copy
the struct without generated per-struct code.

Usage:
  python3 scripts/gen-query-only-structs.py . host/vulkan/cereal/common/goldfish_vk_query_only_structs.h [--list]
"""
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOTS = ("VkPhysicalDeviceFeatures2", "VkPhysicalDeviceProperties2")

SCALARS = {
    "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t", "uint64_t", "int64_t",
    "float", "double", "char",
}
BE64_TYPES = {"size_t"}  # gfxstream streams size_t as a big-endian 64-bit value


def main():
    gfx, out = sys.argv[1], sys.argv[2]
    want_list = "--list" in sys.argv
    reg = ET.parse(os.path.join(gfx, "third_party", "vulkan_docs", "xml", "vk.xml")).getroot()

    types = {}
    for t in reg.find("types").findall("type"):
        name = t.get("name") or (t.find("name").text if t.find("name") is not None else None)
        if not name:
            continue
        api = t.get("api")
        if api and "vulkan" not in api.split(","):
            continue
        types[name] = t

    # Enum aliases for sType canonicalisation.
    enum_alias = {}
    for e in reg.iter("enum"):
        if e.get("alias") and (e.get("name") or "").startswith("VK_STRUCTURE_TYPE_"):
            enum_alias[e.get("name")] = e.get("alias")

    def canon(s):
        for _ in range(5):
            if s not in enum_alias:
                break
            s = enum_alias[s]
        return s

    # Which feature (core version or extension) first requires each type.
    owner = {}
    for f in reg.findall("feature"):
        api = f.get("api", "vulkan")
        if "vulkan" not in api.split(","):
            continue
        for req in f.findall("require"):
            for ty in req.findall("type"):
                owner.setdefault(ty.get("name"), ("core", f.get("name"), f))
    for x in reg.find("extensions").findall("extension"):
        for req in x.findall("require"):
            for ty in req.findall("type"):
                owner.setdefault(ty.get("name"), ("ext", x.get("name"), x))

    handled = set()
    src = open(os.path.join(gfx, "host", "vulkan", "cereal", "common",
                            "goldfish_vk_reserved_marshaling.cpp"),
               encoding="utf-8", errors="replace").read()
    m = re.search(r"^void reservedunmarshal_extension_struct\([^;{]*\)\s*\{", src, re.M)
    body = src[m.end():src.find("\n}\n", m.end())]
    handled = {canon(s) for s in re.findall(r"case (VK_STRUCTURE_TYPE_\w+):", body)}

    def category(name):
        t = types.get(name)
        if t is None:
            return None
        return t.get("category")

    def member_info(mem):
        """Return (type, name, dims, is_pointer, is_bitfield)."""
        ty = mem.find("type").text
        nm = mem.find("name").text
        tail = "".join(mem.itertext())
        is_ptr = "*" in tail
        dims = []
        after_name = (mem.find("name").tail or "")
        enums = [e.text for e in mem.findall("enum")]
        # [N] literal or [<enum>X</enum>] forms
        dims += re.findall(r"\[(\d+)\]", after_name)
        for e in enums:
            dims.append(e)
        is_bitfield = after_name.strip().startswith(":")
        return ty, nm, dims, is_ptr, is_bitfield

    def flatten(tyname, prefix, why):
        """Flatten a plain struct into [(designator, kind)]; None if not plain."""
        t = types[tyname]
        fields = []
        for mem in t.findall("member"):
            api = mem.get("api")
            if api and "vulkan" not in api.split(","):
                continue
            mty, mname, dims, is_ptr, is_bitfield = member_info(mem)
            if mname in ("sType", "pNext") and prefix == "":
                continue
            if mname in ("sType", "pNext"):
                why.append("nested struct with sType/pNext")
                return None
            if is_ptr or is_bitfield:
                why.append("pointer/bitfield member %s" % mname)
                return None
            cat = category(mty)
            desig = prefix + mname
            if mty in SCALARS or cat in ("enum", "bitmask") or mty in ("VkBool32", "VkDeviceSize",
                                                                         "VkDeviceAddress",
                                                                         "VkSampleMask", "VkFlags",
                                                                         "VkFlags64"):
                fields.append((desig, "raw"))
            elif cat == "basetype":
                # VkBool32 / VkDeviceSize / ... are typedefs of fixed-width integers
                fields.append((desig, "raw"))
            elif mty in BE64_TYPES:
                if dims:
                    why.append("array of size_t")
                    return None
                fields.append((desig, "be64"))
            elif cat == "struct":
                if dims:
                    if len(dims) != 1 or not dims[0].isdigit():
                        why.append("array of structs with symbolic length %s" % mname)
                        return None
                    for i in range(int(dims[0])):
                        sub = flatten(mty, "%s[%d]." % (desig, i), why)
                        if sub is None:
                            return None
                        fields += sub
                    continue
                sub = flatten(mty, desig + ".", why)
                if sub is None:
                    return None
                fields += sub
            else:
                why.append("member %s of type %s (%s)" % (mname, mty, cat))
                return None
        return fields

    rows, skipped = [], []
    for name, t in sorted(types.items()):
        if t.get("category") != "struct" or t.get("alias"):
            continue
        ext = (t.get("structextends") or "").split(",")
        if not any(r in ext for r in ROOTS):
            continue
        stype = None
        for mem in t.findall("member"):
            if (mem.get("values") or "").startswith("VK_STRUCTURE_TYPE_"):
                stype = mem.get("values")
        if not stype:
            continue
        if canon(stype) in handled:
            continue
        kind, feat, node = owner.get(name, (None, None, None))
        if kind is None:
            skipped.append((name, "not required by any vulkan feature"))
            continue
        if kind == "ext":
            if node.get("supported") and "vulkan" not in node.get("supported").split(","):
                skipped.append((name, "extension %s not supported for vulkan" % feat))
                continue
            if node.get("platform"):
                skipped.append((name, "platform extension %s" % feat))
                continue
            if node.get("provisional") == "true":
                skipped.append((name, "provisional extension %s" % feat))
                continue
        why = []
        fields = flatten(name, "", why)
        if fields is None:
            skipped.append((name, "; ".join(why)))
            continue
        rows.append((name, stype, feat, fields))

    if want_list:
        for name, stype, feat, fields in rows:
            print("%-64s %-48s %d fields" % (name, feat, len(fields)))
        print("-- skipped:")
        for name, why in skipped:
            print("%-64s %s" % (name, why))
        print("total %d, skipped %d" % (len(rows), len(skipped)))

    lines = []
    w = lines.append
    w("// Copyright 2026 The Android Open Source Project")
    w("//")
    w('// Licensed under the Apache License, Version 2.0 (the "License");')
    w("// you may not use this file except in compliance with the License.")
    w("// You may obtain a copy of the License at")
    w("//")
    w("// http://www.apache.org/licenses/LICENSE-2.0")
    w("//")
    w("// Unless required by applicable law or agreed to in writing, software")
    w('// distributed under the License is distributed on an "AS IS" BASIS,')
    w("// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.")
    w("// See the License for the specific language governing permissions and")
    w("// limitations under the License.")
    w("")
    w("// Feature and property structs that guests may chain into vkGetPhysicalDeviceFeatures2,")
    w("// vkGetPhysicalDeviceProperties2 or vkCreateDevice, but whose extensions gfxstream does not")
    w("// support, so the generated marshaling code has no case for them. Older guest encoders were")
    w("// generated against the whole registry and still send them; without a codec the host aborts.")
    w("//")
    w("// Each entry gives the C layout of the struct's wire fields in wire order. The wire format of")
    w("// these plain-data structs is: sType, the pNext chain, then every field as raw bytes, except")
    w("// size_t which is a big-endian 64-bit value.")
    w("//")
    w("// Generated from vk.xml by scripts/gen-query-only-structs.py; do not edit by hand.")
    w("")
    w("#pragma once")
    w("")
    w("#include <stddef.h>")
    w("#include <stdint.h>")
    w("")
    w("#include <vulkan/vulkan_core.h>")
    w("")
    w("namespace gfxstream {")
    w("namespace host {")
    w("namespace vk {")
    w("")
    w("enum class QueryOnlyFieldKind : uint32_t {")
    w("    kRaw = 0,")
    w("    kSizeT = 1,")
    w("};")
    w("")
    w("struct QueryOnlyField {")
    w("    uint32_t offset;")
    w("    uint32_t size;")
    w("    QueryOnlyFieldKind kind;")
    w("};")
    w("")
    w("struct QueryOnlyStructInfo {")
    w("    VkStructureType sType;")
    w("    uint32_t structSize;")
    w("    const QueryOnlyField* fields;")
    w("    uint32_t fieldCount;")
    w("};")
    w("")
    w("#define GFXSTREAM_QUERY_ONLY_FIELD(T, member, kind)                     \\")
    w("    {static_cast<uint32_t>(offsetof(T, member)),                         \\")
    w("     static_cast<uint32_t>(sizeof(static_cast<T*>(nullptr)->member)), \\")
    w("     QueryOnlyFieldKind::kind}")
    w("")
    for name, stype, feat, fields in rows:
        w("// %s" % feat)
        w("inline constexpr QueryOnlyField k%sFields[] = {" % name)
        for desig, kind in fields:
            w("    GFXSTREAM_QUERY_ONLY_FIELD(%s, %s, %s)," % (
                name, desig, "kSizeT" if kind == "be64" else "kRaw"))
        w("};")
    w("")
    w("#undef GFXSTREAM_QUERY_ONLY_FIELD")
    w("")
    w("inline constexpr QueryOnlyStructInfo kQueryOnlyStructs[] = {")
    for name, stype, feat, fields in rows:
        w("    {%s, sizeof(%s), k%sFields," % (stype, name, name))
        w("     static_cast<uint32_t>(sizeof(k%sFields) / sizeof(QueryOnlyField))}," % name)
    w("};")
    w("")
    w("// Returns the layout of a query-only struct, or nullptr if |structType| is not one.")
    w("inline const QueryOnlyStructInfo* goldfish_vk_query_only_struct_info(uint32_t structType) {")
    w("    for (const QueryOnlyStructInfo& info : kQueryOnlyStructs) {")
    w("        if (static_cast<uint32_t>(info.sType) == structType) {")
    w("            return &info;")
    w("        }")
    w("    }")
    w("    return nullptr;")
    w("}")
    w("")
    w("}  // namespace vk")
    w("}  // namespace host")
    w("}  // namespace gfxstream")
    w("")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s: %d structs" % (out, len(rows)), file=sys.stderr)


if __name__ == "__main__":
    main()
