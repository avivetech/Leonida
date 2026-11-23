#pragma once

// Console rectangle lists on Vulkan, which has no such primitive.
//
// A rectangle is three corners (v1, v2, v3) and the hardware makes the fourth, v2 + v3 - v1, for the position and for
// every interpolant, after the vertex stage. A list topology of rectangles (D3D_PRIMITIVE_TOPOLOGY 17) and a pipeline
// whose topology type is 19 are console-only. The fact comes from xvalkyrie (GPL-3.0, commit 53af7df77c10, notes in
// its backlog); no code is taken from it.
//
// The fourth vertex depends on what the vertex stage wrote, not on what it read, so the expansion has to sit after
// the vertex stage: a pre-pass over the vertex buffers is wrong for any shader that is not linear in its inputs (and
// the translated vertex shaders fetch by V# themselves, so there is no vertex buffer to expand). The pipeline gets a
// small geometry stage instead, built here from the vertex shader's own SPIR-V: it takes the triangle (v1, v2, v3)
// the list draws and emits the parallelogram as the two triangles (v1, v2, v4) and (v1, v4, v3), with the winding of
// (v1, v2, v3). Both start at v1, so a flat input (taken from a triangle's first vertex) is v1's over the whole
// rectangle. Every Location output of the vertex stage is a vec4 float in the translator's output (rdna2spirv.py),
// and so is the stage's Position; the generated stage reads and writes exactly those.
//
// Not covered, and refused with a reason: a vertex stage that writes ClipDistance, CullDistance, Layer or
// ViewportIndex, or a Location output that is not a vec4 float.
//
// Plain word editing, standard library only.

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace leonida {

// D3D_PRIMITIVE_TOPOLOGY values and the PSO topology type of the console's rectangle and quad lists.
constexpr std::uint32_t topology_rect_list = 17;
constexpr std::uint32_t topology_quad_list = 19;
constexpr std::uint32_t topology_type_rect_list = 19;

// The geometry stage for `vertex_words`' outputs; empty, with `why` set, when the vertex stage cannot be expanded.
inline std::vector<std::uint32_t> rect_list_geometry(const std::vector<std::uint32_t>& vertex_words, std::string& why) {
    enum : std::uint32_t {
        op_entry_point = 15, op_type_void = 19, op_type_int = 21, op_type_float = 22,
        op_type_vector = 23, op_type_array = 28, op_type_pointer = 32, op_type_function = 33, op_constant = 43,
        op_function = 54, op_function_end = 56, op_variable = 59, op_load = 61, op_store = 62, op_access_chain = 65,
        op_decorate = 71, op_fadd = 129, op_fsub = 131, op_emit_vertex = 218, op_end_primitive = 219, op_label = 248, op_return = 253,
        op_capability = 17, op_memory_model = 14, op_execution_mode = 16,
        deco_builtin = 11, deco_location = 30, builtin_position = 0, builtin_point_size = 1,
        storage_input = 1, storage_output = 3
    };
    why.clear();
    if (vertex_words.size() < 6 || vertex_words[0] != 0x07230203u) {
        why = "the vertex shader is not a SPIR-V module";
        return {};
    }
    // What the vertex stage declares: types, Output variables, and the decorations that name them.
    struct Type { std::uint32_t op = 0, component = 0, count = 0, width = 0; };
    std::map<std::uint32_t, Type> types;
    std::map<std::uint32_t, std::uint32_t> variable_type;  // Output variable -> its pointer type
    std::map<std::uint32_t, std::uint32_t> location, builtin;       // variable id -> value
    std::set<std::uint32_t> outputs;
    for (std::size_t i = 5; i < vertex_words.size();) {
        const std::uint32_t op = vertex_words[i] & 0xffffu, n = vertex_words[i] >> 16;
        if (!n || i + n > vertex_words.size()) {
            why = "the vertex shader's SPIR-V is cut short";
            return {};
        }
        const std::uint32_t* w = &vertex_words[i];
        if (op == op_type_float && n >= 3) types[w[1]] = {op, 0, 0, w[2]};
        else if (op == op_type_vector && n >= 4) types[w[1]] = {op, w[2], w[3], 0};
        else if (op == op_type_pointer && n >= 4) types[w[1]] = {op, w[3], w[2], 0};
        else if (op == op_variable && n >= 4 && w[3] == storage_output) outputs.insert(w[2]), variable_type[w[2]] = w[1];
        else if (op == op_decorate && n >= 4 && w[2] == deco_location) location[w[1]] = w[3];
        else if (op == op_decorate && n >= 4 && w[2] == deco_builtin) builtin[w[1]] = w[3];
        i += n;
    }
    std::uint32_t position = 0;
    std::vector<std::uint32_t> params;  // Location values, ascending
    for (std::uint32_t id : outputs) {
        const auto b = builtin.find(id);
        if (b != builtin.end()) {
            if (b->second == builtin_position) position = id;
            else if (b->second != builtin_point_size) {
                why = "the vertex shader writes ClipDistance, CullDistance, Layer or ViewportIndex";
                return {};
            }
            continue;
        }
        const auto l = location.find(id);
        if (l == location.end()) continue;
        const Type& ptr = types[variable_type[id]];
        const Type& vec = types[ptr.component];
        const Type& scalar = types[vec.component];
        if (vec.op != op_type_vector || vec.count != 4 || scalar.op != op_type_float || scalar.width != 32) {
            why = "a vertex output is not a vec4 float";
            return {};
        }
        params.push_back(l->second);
    }
    if (!position) {
        why = "the vertex shader writes no Position";
        return {};
    }
    std::sort(params.begin(), params.end());
    params.erase(std::unique(params.begin(), params.end()), params.end());

    // The module: a fixed prelude, then per variable ids, then the body. Ids count up from 1.
    std::uint32_t next = 1;
    const auto id = [&] { return next++; };
    std::vector<std::uint32_t> head, annotations, globals, body;
    const auto emit = [](std::vector<std::uint32_t>& to, std::uint32_t op, std::initializer_list<std::uint32_t> args) {
        to.push_back((std::uint32_t(args.size()) + 1) << 16 | op);
        to.insert(to.end(), args.begin(), args.end());
    };
    const std::uint32_t t_void = id(), t_fn = id(), t_uint = id(), t_float = id(), t_v4 = id(),
                        c3 = id(), t_arr = id(), t_pin_arr = id(), t_pin_v4 = id(),
                        t_pout_v4 = id(), c0 = id(), c1 = id(), c2 = id();
    emit(globals, op_type_void, {t_void});
    emit(globals, op_type_function, {t_fn, t_void});
    emit(globals, op_type_int, {t_uint, 32, 0});
    emit(globals, op_type_float, {t_float, 32});
    emit(globals, op_type_vector, {t_v4, t_float, 4});
    emit(globals, op_constant, {t_uint, c3, 3});
    emit(globals, op_constant, {t_uint, c0, 0});
    emit(globals, op_constant, {t_uint, c1, 1});
    emit(globals, op_constant, {t_uint, c2, 2});
    emit(globals, op_type_array, {t_arr, t_v4, c3});
    emit(globals, op_type_pointer, {t_pin_arr, storage_input, t_arr});
    emit(globals, op_type_pointer, {t_pin_v4, storage_input, t_v4});
    emit(globals, op_type_pointer, {t_pout_v4, storage_output, t_v4});

    struct Var { std::uint32_t in, out; bool is_position; };
    std::vector<Var> vars;
    vars.push_back({id(), id(), true});
    for (std::size_t i = 0; i < params.size(); ++i) vars.push_back({id(), id(), false});
    std::vector<std::uint32_t> io_vars;
    emit(annotations, op_decorate, {vars[0].in, deco_builtin, builtin_position});
    emit(annotations, op_decorate, {vars[0].out, deco_builtin, builtin_position});
    for (std::size_t i = 0; i < params.size(); ++i) {
        emit(annotations, op_decorate, {vars[i + 1].in, deco_location, params[i]});
        emit(annotations, op_decorate, {vars[i + 1].out, deco_location, params[i]});
    }
    for (const Var& v : vars) {
        emit(globals, op_variable, {t_pin_arr, v.in, storage_input});
        emit(globals, op_variable, {t_pout_v4, v.out, storage_output});
        io_vars.push_back(v.in);
        io_vars.push_back(v.out);
    }

    const std::uint32_t main_fn = id(), entry_label = id();
    emit(body, op_function, {t_void, main_fn, 0, t_fn});
    emit(body, op_label, {entry_label});
    // Per variable: the three corners, and the fourth, v2 + v3 - v1.
    struct Corners { std::uint32_t v[4]; };
    std::vector<Corners> corners;
    const std::uint32_t cs[3] = {c0, c1, c2};
    for (const Var& v : vars) {
        Corners c{};
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t ptr = id();
            c.v[k] = id();
            emit(body, op_access_chain, {t_pin_v4, ptr, v.in, cs[k]});
            emit(body, op_load, {t_v4, c.v[k], ptr});
        }
        const std::uint32_t sum = id(), diff = id();
        emit(body, op_fadd, {t_v4, sum, c.v[1], c.v[2]});
        emit(body, op_fsub, {t_v4, diff, sum, c.v[0]});
        c.v[3] = diff;
        corners.push_back(c);
    }
    // Two triangles, (v1, v2, v4) and (v1, v4, v3): the diagonal is v1 to v4, so both start at v1 and a flat input
    // (which Vulkan takes from a triangle's first vertex) is v1's over the whole rectangle. The attributes are
    // affine over the parallelogram, so the split does not change any interpolated value.
    const int triangles[2][3] = {{0, 1, 3}, {0, 3, 2}};
    for (const auto& triangle : triangles) {
        for (int k : triangle) {
            for (std::size_t i = 0; i < vars.size(); ++i) emit(body, op_store, {vars[i].out, corners[i].v[k]});
            emit(body, op_emit_vertex, {});
        }
        emit(body, op_end_primitive, {});
    }
    emit(body, op_return, {});
    emit(body, op_function_end, {});

    std::vector<std::uint32_t> out{0x07230203u, 0x00010000u, 0, next, 0};
    emit(out, op_capability, {2});  // Geometry
    emit(out, op_memory_model, {0, 1});  // Logical, GLSL450
    std::vector<std::uint32_t> entry{3, main_fn, 0x6e69616du, 0};  // Geometry, %main, "main"
    entry.insert(entry.end(), io_vars.begin(), io_vars.end());
    out.push_back((std::uint32_t(entry.size()) + 1) << 16 | op_entry_point);
    out.insert(out.end(), entry.begin(), entry.end());
    emit(out, op_execution_mode, {main_fn, 22});         // Triangles
    emit(out, op_execution_mode, {main_fn, 0, 1});       // Invocations 1
    emit(out, op_execution_mode, {main_fn, 29});         // OutputTriangleStrip
    emit(out, op_execution_mode, {main_fn, 26, 6});      // OutputVertices 6
    out.insert(out.end(), annotations.begin(), annotations.end());
    out.insert(out.end(), globals.begin(), globals.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace leonida
