// Copyright (C) 2023 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "intel_gpu/graph/kernel_impl_params.hpp"
#include "intel_gpu/graph/program.hpp"
#include "intel_gpu/graph/serialization/polymorphic_serializer.hpp"
#include "intel_gpu/graph/serialization/layout_serializer.hpp"
#include "intel_gpu/graph/serialization/string_serializer.hpp"
#include "intel_gpu/graph/serialization/vector_serializer.hpp"
#include "intel_gpu/runtime/device_info.hpp"

#include <string>
#include <vector>

namespace cldnn {

size_t kernel_impl_params::hash() const {
    size_t seed = 0;
    if (desc != nullptr)
        seed = desc->hash();
    const size_t prime_number = 2654435761; // magic number to reduce hash collision rate.
    for (auto& in : input_layouts) {
        seed = hash_combine(seed, in.hash() * prime_number);
    }
    for (auto& out : output_layouts) {
        seed = hash_combine(seed, out.hash() * prime_number);
    }

    // hash for fused prims
    for (auto& fd : fused_desc) {
        seed = hash_combine(seed, fd.desc->hash());
    }

    seed = hash_combine(seed, _can_be_optimized);
    return seed;
}

bool kernel_impl_params::operator==(const kernel_impl_params& rhs) const {
    if ((desc != nullptr && rhs.desc == nullptr) || (desc == nullptr && rhs.desc != nullptr))
        return false;

    if ((desc != nullptr && rhs.desc != nullptr) && *desc != *rhs.desc)
        return false;

    if (rhs.input_layouts.size() != input_layouts.size())
        return false;

    if (rhs.output_layouts.size() != output_layouts.size())
        return false;

    for (size_t i = 0; i < input_layouts.size(); i++) {
        if (input_layouts[i] != rhs.input_layouts[i])
            return false;
    }

    for (size_t i = 0; i < output_layouts.size(); i++) {
        if (output_layouts[i] != rhs.output_layouts[i])
            return false;
    }

    if (fused_desc.size() != rhs.fused_desc.size())
        return false;

    for (size_t i = 0; i < rhs.fused_desc.size(); i++) {
        if (fused_desc[i] != rhs.fused_desc[i])
            return false;
    }

    return true;
}

}  // namespace cldnn
