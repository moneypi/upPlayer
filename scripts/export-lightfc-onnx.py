#!/usr/bin/env python3
"""Export LightFC MobileNetV2 to lightfc_z.onnx + lightfc_x.onnx for upPlayer."""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

import torch
import torch.nn as nn


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", type=Path, required=True, help="LightFC repo root")
    ap.add_argument("--ckpt", type=Path, required=True, help="lightfc_ep####.pth.tar")
    ap.add_argument("--yaml", type=Path, default=None)
    ap.add_argument("--out", type=Path, required=True, help="Output directory")
    args = ap.parse_args()

    src = args.src.resolve()
    sys.path.insert(0, str(src))

    from lib.models import LightFC
    from lib.utils.load import load_yaml

    yaml_path = args.yaml
    if yaml_path is None:
        yaml_path = src / (
            "experiments/lightfc/"
            "mobilnetv2_p_pwcorr_se_scf_sc_iab_sc_adj_concat_repn33_se_conv33_center_wiou.yaml"
        )
    cfg = load_yaml(str(yaml_path))
    network = LightFC(cfg=cfg, env_num=None, training=False)
    ckpt = torch.load(str(args.ckpt), map_location="cpu")
    state = ckpt["net"] if isinstance(ckpt, dict) and "net" in ckpt else ckpt
    network.load_state_dict(state, strict=True)
    for module in network.modules():
        if hasattr(module, "switch_to_deploy"):
            module.switch_to_deploy()
    network.eval()

    class ZWrapper(nn.Module):
        def __init__(self, net):
            super().__init__()
            self.net = net

        def forward(self, z):
            return self.net.forward_backbone(z)

    class XWrapper(nn.Module):
        def __init__(self, net):
            super().__init__()
            self.net = net

        def forward(self, z_feat, x):
            out = self.net.forward_tracking(z_feat, x)
            return out["score_map"], out["size_map"], out["offset_map"]

    args.out.mkdir(parents=True, exist_ok=True)
    z_path = args.out / "lightfc_z.onnx"
    x_path = args.out / "lightfc_x.onnx"

    z = torch.randn(1, 3, 128, 128)
    z_feat = network.forward_backbone(z)
    print("z_feat shape:", tuple(z_feat.shape))

    torch.onnx.export(
        ZWrapper(network),
        z,
        str(z_path),
        input_names=["z"],
        output_names=["z_feat"],
        opset_version=17,
        dynamo=False,
    )
    print("wrote", z_path)

    x = torch.randn(1, 3, 256, 256)
    torch.onnx.export(
        XWrapper(network),
        (z_feat, x),
        str(x_path),
        input_names=["z_feat", "x"],
        output_names=["score_map", "size_map", "offset_map"],
        opset_version=17,
        dynamo=False,
    )
    print("wrote", x_path)

    try:
        import onnx
        from onnxsim import simplify

        for p in (z_path, x_path):
            model = onnx.load(str(p))
            model_simp, ok = simplify(model)
            if ok:
                onnx.save(model_simp, str(p))
                print("simplified", p)
    except Exception as ex:
        print("onnxsim skipped:", ex)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
