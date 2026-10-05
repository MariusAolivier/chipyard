#!/usr/bin/env python3
"""Lower a supported DORY NE16 layer to single-core cooperative jobs."""

import argparse
import json
import re
import tempfile
from pathlib import Path

from dory_ne16_import import c_array, parse_expected, sha256


DORY_COMMIT = "add0d9c1be889b5f802b2606ced8c59acff8aa02"
TILE_HEIGHT = TILE_WIDTH = 12
CHANNELS = 16
SLOT_BYTES = 16384
OFFSETS = {"input": 2048, "output": 8192, "weights": 12288,
           "scale": 14592, "bias": 14656}
DMA_FIELDS = ("src", "dst", "line_bytes", "rows", "planes",
              "src_row_stride", "dst_row_stride",
              "src_plane_stride", "dst_plane_stride")
HEADERS = ("BNReluConvolution0.h", "dory_get_tile.h", "execute.h",
           "layer.h", "tile_index.h", "tile_status.h")


def validate_namespace(namespace: str) -> None:
    if (type(namespace) is not str or
            not re.fullmatch(r"[a-z][a-z0-9_]{0,47}", namespace, flags=re.ASCII)):
        raise ValueError("namespace must be a lowercase C identifier of at most 48 characters")


def namespace_binding(text: str, namespace: str) -> str:
    validate_namespace(namespace)
    text = re.sub(r"\bdory_generated_(\w+)\b",
                  lambda match: namespace + "_" + match[1], text)
    return re.sub(r"\bDORY_GENERATED_(\w+)\b",
                  lambda match: namespace.upper() + "_" + match[1], text)


def uncomment(source: str) -> str:
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)


def balanced(source: str, start: int, opening: str, closing: str) -> tuple[str, int]:
    if start >= len(source) or source[start] != opening:
        raise ValueError(f"expected {opening}")
    depth = 0
    for index in range(start, len(source)):
        if source[index] == opening:
            depth += 1
        elif source[index] == closing:
            depth -= 1
            if depth == 0:
                return source[start + 1:index], index + 1
    raise ValueError(f"unterminated {opening}")


def initializer(text: str) -> dict:
    tokens = re.findall(r"[A-Za-z_]\w*|-?\d+|[{}.=,]", text)
    if re.sub(r"\s+", "", "".join(tokens)) != re.sub(r"\s+", "", text):
        raise ValueError("nonliteral generated initializer")
    position = 0

    def take(expected=None):
        nonlocal position
        if position >= len(tokens):
            raise ValueError("truncated generated initializer")
        token = tokens[position]
        position += 1
        if expected is not None and token != expected:
            raise ValueError(f"expected {expected}, found {token}")
        return token

    def fields(nested=False):
        result = {}
        while position < len(tokens) and tokens[position] != "}":
            take(".")
            name = take()
            if not re.fullmatch(r"[A-Za-z_]\w*", name) or name in result:
                raise ValueError("invalid or duplicate initializer field")
            take("=")
            if position >= len(tokens):
                raise ValueError("truncated generated initializer")
            if tokens[position] == "{":
                take("{")
                value = fields(True)
                take("}")
            else:
                value = take()
                if re.fullmatch(r"-?\d+", value):
                    if not re.fullmatch(r"-?(?:0|[1-9]\d*)", value):
                        raise ValueError("nondecimal generated initializer")
                    value = int(value)
            result[name] = value
            if position < len(tokens) and tokens[position] == ",":
                take(",")
            elif position < len(tokens) and tokens[position] != "}":
                raise ValueError("missing initializer separator")
        if not nested and position != len(tokens):
            raise ValueError("unexpected initializer suffix")
        return result

    return fields()


def declaration(source: str, kind: str, name: str) -> dict:
    matches = list(re.finditer(rf"static\s+const\s+{kind}\s+{name}\s*=\s*\{{", source))
    if len(matches) != 1:
        raise ValueError(f"expected one generated {name} declaration")
    match = matches[0]
    body, end = balanced(source, match.end() - 1, "{", "}")
    if not re.match(r"\s*;", source[end:]):
        raise ValueError(f"invalid generated {name} terminator")
    return initializer(body)


def arguments(text: str) -> list[str]:
    result, start, depth = [], 0, 0
    for index, char in enumerate(text):
        if char in "({[":
            depth += 1
        elif char in ")}]":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:index].strip())
            start = index + 1
        if depth < 0:
            raise ValueError("invalid generated call")
    if depth != 0:
        raise ValueError("unterminated generated call")
    result.append(text[start:].strip())
    return result


def call(source: str, name: str) -> list[str]:
    matches = list(re.finditer(rf"\b{name}\s*\(", source))
    if len(matches) != 1:
        raise ValueError(f"expected one generated {name} call")
    body, _ = balanced(source, matches[0].end() - 1, "(", ")")
    return arguments(body)


def function_body(source: str, name: str) -> str:
    matches = list(re.finditer(rf"\b{name}\s*\([^)]*\)\s*\{{", source))
    if len(matches) != 1:
        raise ValueError(f"expected one generated {name} definition")
    return balanced(source, matches[0].end() - 1, "{", "}")[0]


def assignment(source: str, name: str, expected: str) -> None:
    values = re.findall(rf"(?<!\w){re.escape(name)}\s*=\s*([^;]+);", source)
    if len(values) != 1 or re.sub(r"\s+", "", values[0]) != expected:
        raise ValueError(f"unsupported generated assignment {name}")


def validate_config(config: dict) -> tuple[int, int, int]:
    if not isinstance(config, dict):
        raise ValueError("configuration must be a JSON object")
    required = {
        "layer_type": "Convolution", "operation_type": "Conv",
        "input_channels": 16, "kernel_shape": [3, 3],
        "padding": [1, 1, 1, 1], "stride": [1, 1], "group": 1,
        "input_bits": 8, "input_type": "uint",
        "output_bits": 8, "output_type": "uint",
        "weight_bits": 8, "bias_bits": 32, "BNRelu_bits": 32,
        "intermediate_bits": 32, "batchnorm": True,
        "branch_in": 0, "branch_out": 1, "branch_change": 0,
    }
    for name, expected in required.items():
        value = config.get(name)
        if (type(value) is not type(expected) or value != expected or
                isinstance(expected, list) and
                any(type(actual) is not type(wanted)
                    for actual, wanted in zip(value, expected))):
            raise ValueError(f"unsupported configuration {name}: {value!r}")
    shape = config.get("input_dimensions")
    channels = config.get("output_channels")
    if (not isinstance(shape, list) or len(shape) != 2 or
            any(type(value) is not int or not 1 <= value <= 65535 for value in shape) or
            type(channels) is not int or not 1 <= channels <= 65535 or
            channels % CHANNELS != 0):
        raise ValueError("invalid full-layer dimensions/channels")
    return shape[0], shape[1], channels


def validate_generated(source: str, execute: str, height: int,
                       width: int, channels: int, use_generated_shift: bool = False) -> dict:
    source, execute = uncomment(source), uncomment(execute)
    if "pulp_nn_conv_" in source or '#include "execute.h"' not in source:
        raise ValueError("not a DORY NE16 generated layer")
    if declaration(source, "Kernel", "kernel") != {
            "shape": {"height": 3, "width": 3},
            "stride": {"height": 1, "width": 1}, "groups": 1}:
        raise ValueError("unsupported generated kernel")
    end = declaration(source, "TileIndex", "end_index")
    body = declaration(source, "Layer", "body")
    border = declaration(source, "Layer", "border")
    if set(end) != {"height", "width", "output_channel"}:
        raise ValueError("invalid generated tile grid")
    if any(type(value) is not int or value <= 0 for value in end.values()):
        raise ValueError("invalid generated tile count")
    for layer in (body, border):
        if set(layer) != {"input", "output"}:
            raise ValueError("unsupported generated tile fields")
        for dimensions in layer.values():
            if (not isinstance(dimensions, dict) or
                    set(dimensions) != {"height", "width", "channel"} or
                    any(type(value) is not int or value <= 0 for value in dimensions.values())):
                raise ValueError("invalid generated tile dimensions")
        if layer["input"]["channel"] != CHANNELS:
            raise ValueError("input-channel tiling is unsupported")
    for axis, full in (("height", height), ("width", width), ("channel", channels)):
        grid = end["output_channel" if axis == "channel" else axis]
        if ((grid - 1) * body["output"][axis] + border["output"][axis] != full or
                border["output"][axis] > body["output"][axis]):
            raise ValueError(f"generated grid does not cover full {axis}")
    for layer in (body, border):
        if layer["output"]["channel"] % CHANNELS != 0:
            raise ValueError("partial generated output-channel groups are unsupported")
    for axis, full in (("height", height), ("width", width)):
        tiled = end[axis] > 1
        if (body["input"][axis] != (body["output"][axis] + 2 if tiled else full) or
                border["input"][axis] != (border["output"][axis] + 1 if tiled else full)):
            raise ValueError(f"unsupported generated input {axis} geometry")
    for name, direction, tensor in (("load_input_prepare", "1", "input"),
                                    ("store_prepare", "0", "output")):
        transfer = function_body(source, name)
        assignment(transfer, "conf->stride_2d", str(width * (CHANNELS if direction == "1" else channels)))
        assignment(transfer, "conf->stride_1d", str(CHANNELS if direction == "1" else channels))
        assignment(transfer, "conf->dir", direction)
        address = [re.sub(r"\s+", "", value) for value in call(transfer, "dory_get_tile_3d")]
        expected_address = [
            f"layer.addr.{tensor}", "index.height", "index.width",
            "kernel.groups>1?index.output_channel:0" if direction == "1" else "index.output_channel",
            f"body.{tensor}.height", f"body.{tensor}.width", f"body.{tensor}.channel",
            str(width), str(CHANNELS if direction == "1" else channels),
            "2" if direction == "1" else "0", "2" if direction == "1" else "0", "0",
            "x_offset_h" if direction == "1" else "0",
            "x_offset_w" if direction == "1" else "0", "0", "8"]
        if address != expected_address:
            raise ValueError(f"unsupported generated {tensor} address/stride semantics")
        for field, value in (
                ("number_of_2d_copies", f"tile.{tensor}.height"),
                ("number_of_1d_copies", f"tile.{tensor}.width"),
                ("length_1d_copy", f"tile.{tensor}.channel"),
                ("loc", f"tile.addr.{tensor}")):
            assignment(transfer, "conf->" + field, value)
    weight_transfer = function_body(source, "load_weights_prepare")
    assignment(weight_transfer, "size_weights",
               "(kernel.groups>1?divnceil(tile.output.channel,16):tile.output.channel)*144")
    assignment(weight_transfer, "size_scale", "tile.output.channel*4")
    assignment(weight_transfer, "size_bias", "tile.output.channel*4")
    layer_match = re.search(r"\bLayer\s+layer\s*=\s*\{", function_body(source, "layer_task_fork"))
    if layer_match is None:
        raise ValueError("missing generated full-layer addresses")
    worker = function_body(source, "layer_task_fork")
    addresses, _ = balanced(worker, layer_match.end() - 1, "{", "}")
    expected_addresses = (
        ".addr={.input=layer_args->L2_input,.weights=layer_args->L2_weights,"
        f".scale=layer_args->L2_weights+{channels * 144},"
        f".bias=layer_args->L2_weights+{channels * 148},"
        ".output=layer_args->L2_output},.padding={"
        ".top=layer_args->padding&NET_UTILS_PAD_TOP?1:NE16_DONT_PAD,"
        ".right=1,.bottom=layer_args->padding&NET_UTILS_PAD_BOTTOM?1:NE16_DONT_PAD,"
        ".left=1}")
    if re.sub(r"\s+", "", addresses) != expected_addresses:
        raise ValueError("unsupported generated normalization offsets/padding")
    task = call(function_body(source, "BNReluConvolution0"), "nnx_task_init")
    compact = lambda value: re.sub(r"\s+", "", value)
    if (len(task) != 11 or compact(task[0]) != "&nnx_tasks[i]" or
            [compact(value) for value in task[1:8]] !=
            ["3", "0", "8", "8", "8", "weightOffsetModeLayerWise", "-128"] or
            compact(task[10]) != "1"):
        raise ValueError("unsupported generated NNX task mode")
    quant_match = re.fullmatch(r"\(\s*nnx_quant_t\s*\)\s*\{(.*)\}", task[8], re.S)
    if quant_match is None:
        raise ValueError("unsupported generated nnx_quant_t")
    quantization = initializer(quant_match[1])
    shift = quantization.get("shift_amount")
    if (type(shift) is not int or not 0 <= shift <= 31 or
            not use_generated_shift and shift != 11):
        raise ValueError("unsupported generated quantization shift")
    for text, kind, expected in (
            (task[8], "nnx_quant_t", {"shift_amount": shift, "mode": "quantMode8Bit",
             "function": "quantFunctionRelu", "flag_rounding": "NE16_FLAG_UNUSED"}),
            (task[9], "nnx_norm_t", {"mode": "normMode32Bit",
             "flag_bias": "NE16_FLAG_USED", "flag_shift": "NE16_FLAG_UNUSED"})):
        match = re.fullmatch(rf"\(\s*{kind}\s*\)\s*\{{(.*)\}}", text, re.S)
        if match is None or initializer(match.group(1)) != expected:
            raise ValueError(f"unsupported generated {kind}")
    statements = function_body(execute, "execute_prepare")
    expected = (
        "nnx_task_set_dims(task,tile.input.width,tile.input.channel,"
        "tile.input.width,tile.input.channel,tile.output.height,tile.output.width,"
        "tile.output.channel,tile.output.width,tile.output.channel,"
        "tile.padding.top,tile.padding.bottom,tile.padding.right,tile.padding.left);"
        "nnx_task_set_ptrs(task,tile.addr.input,tile.input.width,tile.input.channel,"
        "8,tile.padding.top,tile.padding.left,tile.addr.output,tile.addr.weights,"
        "tile.addr.scale,0,tile.addr.bias);")
    if compact(statements) != expected:
        raise ValueError("unsupported generated execute_prepare semantics")
    for function in ("nnx_dispatch_task", "nnx_resolve_check"):
        if re.search(rf"\b{function}\s*\(", execute) is None:
            raise ValueError(f"missing generated {function}")
    result = {"grid": end, "body": body, "border": border,
              "jobs": end["height"] * end["width"] * end["output_channel"]}
    if use_generated_shift:
        result["quantization_shift"] = shift
    return result


def make_plan(height: int, width: int, channels: int) -> list[dict]:
    result = []
    for y in range(0, height, TILE_HEIGHT):
        for x in range(0, width, TILE_WIDTH):
            for channel in range(0, channels, CHANNELS):
                h, w = min(TILE_HEIGHT, height - y), min(TILE_WIDTH, width - x)
                top, left = int(y == 0), int(x == 0)
                bottom, right = int(y + h == height), int(x + w == width)
                iy, ix = y - (1 - top), x - (1 - left)
                ih, iw = h + 2 - top - bottom, w + 2 - left - right

                def load(offset, destination, length):
                    return dict(src=offset, dst=OFFSETS[destination],
                                line_bytes=length, rows=1, planes=1)

                result.append({
                    "output_y": y, "output_x": x, "output_channel": channel,
                    "input_y": iy, "input_x": ix,
                    "input_height": ih, "input_width": iw,
                    "output_height": h, "output_width": w,
                    "padding": dict(top=top, bottom=bottom, left=left, right=right),
                    "loads": [
                        dict(src=(iy * width + ix) * CHANNELS, dst=OFFSETS["input"],
                             line_bytes=iw * CHANNELS, rows=ih, planes=1,
                             src_row_stride=width * CHANNELS,
                             dst_row_stride=iw * CHANNELS),
                        load(channel * 144, "weights", 2304),
                        load(channels * 144 + channel * 4, "scale", 64),
                        load(channels * 148 + channel * 4, "bias", 64)],
                    "store": dict(src=OFFSETS["output"],
                                  dst=(y * width + x) * channels + channel,
                                  line_bytes=CHANNELS, rows=w, planes=h,
                                  src_row_stride=CHANNELS, dst_row_stride=channels,
                                  src_plane_stride=w * CHANNELS,
                                  dst_plane_stride=width * channels)})
    return result


def dma_initializer(config: dict) -> str:
    return "{" + ", ".join(f".{name} = {config.get(name, 0)}u" for name in DMA_FIELDS) + "}"


def emit_binding(plan: list[dict], quantization_shift: int = 11) -> str:
    recipes = []
    for recipe in plan:
        tile = (
            "{.addr = {" + ", ".join(f".{name} = {value}u" for name, value in OFFSETS.items()) +
            "}, .input = {" +
            f".height = {recipe['input_height']}, .width = {recipe['input_width']}, .channel = 16" +
            "}, .output = {" +
            f".height = {recipe['output_height']}, .width = {recipe['output_width']}, .channel = 16" +
            "}, .padding = {" +
            ", ".join(f".{name} = {value}" for name, value in recipe["padding"].items()) + "}}")
        recipes.append("  {.tile = " + tile + ",\n   .loads = {" +
                       ",\n             ".join(dma_initializer(load) for load in recipe["loads"]) +
                       "},\n   .store = " + dma_initializer(recipe["store"]) + "}")
    return """/* Generated by import-dory-ne16-cooperative-layer.py; do not edit. */
#include "dory_generated_layer.h"
#include "dory_tiled_layer.h"
#include "execute.h"
#include "ne16_driver.h"

#include <stddef.h>

_Static_assert(DORY_NE16_TILED_SLOT_BYTES == 16384u &&
    DORY_NE16_TILED_INPUT_OFFSET == 2048u &&
    DORY_NE16_TILED_OUTPUT_OFFSET == 8192u &&
    DORY_NE16_TILED_WEIGHTS_OFFSET == 12288u &&
    DORY_NE16_TILED_SCALE_OFFSET == 14592u &&
    DORY_NE16_TILED_BIAS_OFFSET == 14656u,
    "regenerate this plan after changing the scratch slot layout");

typedef struct {
  Layer tile;
  ne16_dma_config_t loads[4], store;
} recipe_t;

static const recipe_t recipes[DORY_GENERATED_JOBS] = {
""" + ",\n".join(recipes) + """
};

int dory_generated_layer_init(dory_generated_layer_t *layer,
                              uint32_t input, uint32_t weights, uint32_t output) {
  if (layer == NULL) return DORY_PIPELINE_EINVAL;
  dory_ne16_tiled_layer_t checked;
  int result = dory_ne16_tiled_init(&checked, input, weights, output,
      DORY_GENERATED_HEIGHT, DORY_GENERATED_WIDTH, DORY_GENERATED_CHANNELS);
  if (result != 0) return result;
  *layer = (dory_generated_layer_t){input, weights, output};
  return 0;
}

int dory_generated_layer_prepare(void *context, uint32_t index, unsigned int slot,
                                dory_pipeline_memory_t memory,
                                dory_pipeline_job_t *job) {
  const dory_generated_layer_t *layer = context;
  dory_generated_layer_t checked;
  if (layer == NULL || job == NULL || index >= DORY_GENERATED_JOBS ||
      slot >= DORY_PIPELINE_SLOTS || memory.bytes < DORY_NE16_TILED_SLOT_BYTES ||
      memory.base < NE16_SCRATCH_BASE ||
      (uint64_t)memory.base + memory.bytes > NE16_SCRATCH_BASE + NE16_SCRATCH_BYTES ||
      dory_generated_layer_init(&checked, layer->input, layer->weights,
                                layer->output) != 0)
    return DORY_PIPELINE_EINVAL;
  const recipe_t *recipe = &recipes[index];
  Layer tile = recipe->tile;
  tile.addr.input += memory.base;
  tile.addr.output += memory.base;
  tile.addr.weights += memory.base;
  tile.addr.scale += memory.base;
  tile.addr.bias += memory.base;
  dory_pipeline_job_t result = {.load_count = 4};
  nnx_task_init(&result.task, 3, 0, 8, 8, 8, weightOffsetModeLayerWise, -128,
      (nnx_quant_t){.shift_amount = """ + str(quantization_shift) + """, .mode = quantMode8Bit,
                   .function = quantFunctionRelu, .flag_rounding = NE16_FLAG_UNUSED},
      (nnx_norm_t){.mode = normMode32Bit, .flag_bias = NE16_FLAG_USED,
                  .flag_shift = NE16_FLAG_UNUSED}, 1);
  execute_prepare(tile, &result.task);
  if (dory_ne16_compat_error() != 0) return DORY_PIPELINE_EPREPARE;
  for (unsigned int i = 0; i < result.load_count; ++i) {
    result.loads[i] = recipe->loads[i];
    result.loads[i].src += i == 0 ? layer->input : layer->weights;
    result.loads[i].dst += memory.base;
  }
  result.store = recipe->store;
  result.store.src += memory.base;
  result.store.dst += layer->output;
  *job = result;
  return 0;
}
"""


def import_layer(args) -> dict:
    namespace = getattr(args, "namespace", "dory_generated")
    validate_namespace(namespace)
    use_generated_shift = getattr(args, "use_generated_shift", False)
    if use_generated_shift and namespace == "dory_generated":
        raise ValueError("generated shifts require an explicit nondefault namespace")
    if args.dory_commit != DORY_COMMIT:
        raise ValueError("unsupported DORY revision; validate its templates before upgrading")
    config_bytes = args.config.read_bytes().replace(b"\r\n", b"\n")
    if sha256(config_bytes) != args.config_sha256:
        raise ValueError("generation config hash mismatch (canonical LF bytes)")
    height, width, channels = validate_config(json.loads(config_bytes))
    source_path = args.generated_dir / "src" / "BNReluConvolution0.c"
    originals = {"src/BNReluConvolution0.c": source_path.read_bytes(),
                 "HW_description.json": args.hardware_description.read_bytes()}
    for name in HEADERS:
        originals["inc/" + name] = (args.generated_dir / "inc" / name).read_bytes()
    upstream = validate_generated(originals["src/BNReluConvolution0.c"].decode(),
                                  originals["inc/execute.h"].decode(),
                                  height, width, channels, use_generated_shift)
    shift = upstream.get("quantization_shift", 11)
    if upstream["jobs"] <= 1:
        raise ValueError("expected a genuinely multi-tile DORY layer")
    tensors = {"input": args.input.read_bytes(), "weights": args.weights.read_bytes(),
               "expected": args.expected.read_bytes()}
    if args.expected_format == "text":
        tensors["expected"] = parse_expected(tensors["expected"])
    lengths = {"input": height * width * CHANNELS,
               "weights": channels * 152, "expected": height * width * channels}
    for name, data in tensors.items():
        if len(data) != lengths[name]:
            raise ValueError(f"{name} size {len(data)} does not match {lengths[name]}")
        if len(data) > 0x80000000:
            raise ValueError(f"{name} cannot fit the 32-bit DRAM address space")
    plan = make_plan(height, width, channels)
    plan_bytes = (json.dumps(plan, indent=2) + "\n").encode()
    header = """/* Generated by import-dory-ne16-cooperative-layer.py; do not edit. */
#ifndef DORY_GENERATED_LAYER_H
#define DORY_GENERATED_LAYER_H
#include "dory_pipeline.h"
"""
    for name, value in (("HEIGHT", height), ("WIDTH", width), ("CHANNELS", channels),
                        ("JOBS", len(plan)), ("INPUT_BYTES", lengths["input"]),
                        ("WEIGHTS_BYTES", lengths["weights"]),
                        ("OUTPUT_BYTES", lengths["expected"])):
        header += f"#define DORY_GENERATED_{name} {value}u\n"
    if use_generated_shift:
        header += f"#define DORY_GENERATED_QUANT_SHIFT {shift}u\n"
    header += """
typedef struct { uint32_t input, weights, output; } dory_generated_layer_t;
extern const uint8_t dory_generated_input[DORY_GENERATED_INPUT_BYTES];
extern const uint8_t dory_generated_weights[DORY_GENERATED_WEIGHTS_BYTES];
extern const uint8_t dory_generated_expected[DORY_GENERATED_OUTPUT_BYTES];
int dory_generated_layer_init(dory_generated_layer_t *layer,
                              uint32_t input, uint32_t weights, uint32_t output);
int dory_generated_layer_prepare(void *context, uint32_t index, unsigned int slot,
                                dory_pipeline_memory_t memory,
                                dory_pipeline_job_t *job);
#endif
"""
    metadata = {
        "schema_version": 1, "dory_commit": args.dory_commit,
        "config": args.config_name, "config_sha256": sha256(config_bytes),
        "config_hash_normalization": "LF",
        "dimensions": {"height": height, "width": width, "input_channels": CHANNELS,
                       "output_channels": channels},
        "upstream": upstream, "cooperative_jobs": len(plan),
        "cooperative_tile": {"height": TILE_HEIGHT, "width": TILE_WIDTH, "channels": CHANNELS},
        "slot_bytes": SLOT_BYTES, "slot_offsets": OFFSETS,
        "plan_sha256": sha256(plan_bytes),
        "source_sha256": {name: sha256(data) for name, data in originals.items()},
        "tensor_sha256": {name: sha256(data) for name, data in tensors.items()},
    }
    if namespace != "dory_generated":
        metadata["namespace"] = namespace
    if use_generated_shift:
        metadata["quantization_shift"] = shift
    basename = namespace + "_layer"
    data_source = (
        '/* Generated by import-dory-ne16-cooperative-layer.py; do not edit. */\n'
        '#include "dory_generated_layer.h"\n\n' +
        "\n\n".join(c_array("dory_generated_" + name, data)
                    for name, data in tensors.items()) + "\n")
    files = {"upstream/" + name: data for name, data in originals.items()}
    files.update({
        basename + ".h": namespace_binding(header, namespace).encode(),
        basename + ".c": namespace_binding(emit_binding(plan, shift), namespace).encode(),
        basename + "_data.c": namespace_binding(data_source, namespace).encode(),
        "cooperative_plan.json": plan_bytes,
        "generated_layer_metadata.json": (json.dumps(metadata, indent=2) + "\n").encode(),
    })
    if args.output_dir.exists():
        raise ValueError("output directory already exists; import into a fresh directory")
    args.output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".dory-import-", dir=args.output_dir.parent) as temporary:
        staging = Path(temporary)
        for name, data in files.items():
            destination = staging / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        if args.output_dir.exists():
            raise ValueError("output directory appeared during import")
        staging.rename(args.output_dir)
    return metadata


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated-dir", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--hardware-description", required=True, type=Path)
    parser.add_argument("--config-name", required=True)
    parser.add_argument("--config-sha256", required=True)
    parser.add_argument("--dory-commit", required=True)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--weights", required=True, type=Path)
    parser.add_argument("--expected", required=True, type=Path)
    parser.add_argument("--expected-format", choices=("text", "binary"), default="text")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--namespace", default="dory_generated")
    parser.add_argument("--use-generated-shift", action="store_true",
                        help="Import DORY's 0..31 shift for an explicit nondefault namespace")
    args = parser.parse_args()
    try:
        metadata = import_layer(args)
    except (ValueError, OSError, UnicodeError) as error:
        parser.error(str(error))
    print(f"Imported {metadata['upstream']['jobs']} upstream DORY tiles as "
          f"{metadata['cooperative_jobs']} cooperative jobs")


if __name__ == "__main__":
    main()
