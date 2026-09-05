#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_tool/issue_card.py

NFC 考勤系统第二阶段上位机工具第一版：
- 生成 704 字节图像 payload
  avatar: 48x64 1bit = 384 bytes = 24 blocks
  name  : 80x16 1bit = 160 bytes = 10 blocks
  dept  : 80x16 1bit = 160 bytes = 10 blocks
- 支持 dump-bin 导出 payload
- 支持 preview 导出预览 PNG
- 支持通过 pyserial 发送 704 字节 payload 到 MCU

依赖：
  pip install pillow

示例：
  python pc_tool/issue_card.py ^
    --avatar ./avatar.jpg ^
    --name 张三 ^
    --dept 电子信息 ^
    --dump-bin ./payload.bin ^
    --preview ./preview.png

如果 MCU/OLED 显示函数使用不同位图字节序，可通过 --pack 切换：
  --pack page-lsb   常见 SSD1306 页格式：每字节 8 个竖向像素，bit0 为上方像素，默认
  --pack page-msb   每字节 8 个竖向像素，bit7 为上方像素
  --pack row-msb    行扫描：每字节 8 个横向像素，bit7 为左侧像素
  --pack row-lsb    行扫描：每字节 8 个横向像素，bit0 为左侧像素

约定：
  - payload 内 bit=1 表示 OLED 像素点亮 / 前景像素
  - 文本默认白字黑底，适合 OLED
  - 头像默认亮区域点亮，可用 --invert-avatar 反相
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Literal

from PIL import Image, ImageDraw, ImageFont, ImageOps


AVATAR_SIZE = (48, 64)
TEXT_SIZE = (80, 16)

AVATAR_BYTES = 384
TEXT_BYTES = 160
PAYLOAD_BYTES = 704
BLOCK_SIZE = 16
PAYLOAD_BLOCKS = 44

PackMode = Literal["page-lsb", "page-msb", "row-msb", "row-lsb"]


@dataclass(frozen=True)
class CardImageBlock:
    index: int
    region: str
    sector: int
    block: int


def build_card_image_block_map() -> list[CardImageBlock]:
    """
    只映射 44 个图像数据块，不包含账户头。
    账户头位于：sector 0, block 1。
    """
    result: list[CardImageBlock] = []
    idx = 0

    # avatar: sectors 1~8, block 0/1/2 = 24 blocks
    for sector in range(1, 9):
        for block in (0, 1, 2):
            result.append(CardImageBlock(idx, "avatar", sector, block))
            idx += 1

    # name: sectors 9~11 block 0/1/2 = 9 blocks, sector 12 block 0 = 1 block
    for sector in range(9, 12):
        for block in (0, 1, 2):
            result.append(CardImageBlock(idx, "name", sector, block))
            idx += 1
    result.append(CardImageBlock(idx, "name", 12, 0))
    idx += 1

    # dept: sector 12 block 1/2 = 2 blocks,
    #       sectors 13~14 block 0/1/2 = 6 blocks,
    #       sector 15 block 0/1 = 2 blocks
    for block in (1, 2):
        result.append(CardImageBlock(idx, "dept", 12, block))
        idx += 1
    for sector in range(13, 15):
        for block in (0, 1, 2):
            result.append(CardImageBlock(idx, "dept", sector, block))
            idx += 1
    for block in (0, 1):
        result.append(CardImageBlock(idx, "dept", 15, block))
        idx += 1

    assert len(result) == PAYLOAD_BLOCKS, len(result)
    return result


CARD_IMAGE_BLOCK_MAP = build_card_image_block_map()


def ensure_parent_dir(path: Path) -> None:
    if path.parent and str(path.parent) != ".":
        path.parent.mkdir(parents=True, exist_ok=True)


def clamp_u8(value: int) -> int:
    return max(0, min(255, int(value)))


def crc16_xmodem(data: bytes) -> int:
    """
    CRC-16/XMODEM:
      poly=0x1021, init=0x0000, refin=false, refout=false, xorout=0x0000

    该变体与串口发卡前置设计报告统一。
    标准测试向量：b"123456789" -> 0x31C3。
    """
    crc = 0x0000
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


def candidate_cjk_fonts() -> list[Path]:
    """
    尽量自动寻找支持中文的字体。
    Windows 常见：微软雅黑 / 黑体 / 宋体
    macOS 常见：苹方 / 冬青黑体
    Linux 常见：Noto CJK / 思源黑体 / 文泉驿
    """
    windir = Path(os.environ.get("WINDIR", r"C:\Windows"))
    candidates = [
        windir / "Fonts" / "msyh.ttc",
        windir / "Fonts" / "msyhbd.ttc",
        windir / "Fonts" / "simhei.ttf",
        windir / "Fonts" / "simsun.ttc",
        windir / "Fonts" / "simkai.ttf",
        Path("/System/Library/Fonts/PingFang.ttc"),
        Path("/System/Library/Fonts/STHeiti Light.ttc"),
        Path("/Library/Fonts/Arial Unicode.ttf"),
        Path("/Library/Fonts/Songti.ttc"),
        Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
        Path("/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf"),
        Path("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc"),
        Path("/usr/share/fonts/truetype/wqy/wqy-microhei.ttc"),
        Path("/usr/share/fonts/truetype/arphic/ukai.ttc"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
    ]

    # 再做少量 glob，兼容不同 Linux 发行版字体路径
    glob_roots = [
        Path("/usr/share/fonts"),
        Path("/usr/local/share/fonts"),
        Path.home() / ".fonts",
        Path.home() / ".local/share/fonts",
    ]
    keywords = (
        "NotoSansCJK",
        "NotoSerifCJK",
        "SourceHanSans",
        "SourceHanSerif",
        "WenQuanYi",
        "wqy",
        "DroidSansFallback",
    )
    for root in glob_roots:
        if root.exists():
            for suffix in ("*.ttf", "*.ttc", "*.otf"):
                for p in root.rglob(suffix):
                    name = p.name
                    if any(k.lower() in name.lower() for k in keywords):
                        candidates.append(p)

    # 去重，保持顺序
    seen: set[str] = set()
    unique: list[Path] = []
    for p in candidates:
        key = str(p)
        if key not in seen:
            seen.add(key)
            unique.append(p)
    return unique


def resolve_font_path(user_font: str | None) -> Path | None:
    if user_font:
        p = Path(user_font).expanduser()
        if not p.exists():
            raise FileNotFoundError(f"字体文件不存在：{p}")
        return p

    for p in candidate_cjk_fonts():
        if p.exists():
            return p
    return None


def load_font(font_path: Path | None, size: int) -> ImageFont.ImageFont:
    if font_path is not None:
        return ImageFont.truetype(str(font_path), size=size)

    # Pillow 新版本 load_default 支持 size，旧版本不支持。
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def has_non_ascii(text: str) -> bool:
    return any(ord(ch) > 127 for ch in text)


def text_bbox(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont) -> tuple[int, int, int, int]:
    bbox = draw.textbbox((0, 0), text, font=font)
    left, top, right, bottom = bbox
    return int(left), int(top), int(right), int(bottom)


def binarize_l_image(gray: Image.Image, threshold: int, dither: bool = False) -> Image.Image:
    """
    输入 L 图，输出 L 图：
      0   = off/background
      255 = on/foreground
    """
    gray = gray.convert("L")
    threshold = clamp_u8(threshold)

    if dither:
        return gray.convert("1", dither=Image.Dither.FLOYDSTEINBERG).convert("L")

    return gray.point(lambda p: 255 if p >= threshold else 0).convert("L")


def render_text_bitmap(
    text: str,
    size: tuple[int, int] = TEXT_SIZE,
    font_path: Path | None = None,
    threshold: int = 128,
    margin_x: int = 0,
    invert: bool = False,
) -> Image.Image:
    """
    渲染 80x16 文本位图。
    逻辑图像：黑底白字，白色为 bit=1。
    """
    width, height = size
    margin_x = max(0, min(margin_x, max(0, width // 2 - 1)))
    max_w = max(1, width - margin_x * 2)
    max_h = height

    if text is None:
        text = ""
    text = str(text)

    canvas = Image.new("L", size, 0)

    if text == "":
        return canvas

    probe = Image.new("L", size, 0)
    probe_draw = ImageDraw.Draw(probe)

    selected_font: ImageFont.ImageFont | None = None
    selected_bbox: tuple[int, int, int, int] | None = None

    # 80x16 空间很小，优先找能完整放下的最大字号。
    for font_size in range(height, 5, -1):
        font = load_font(font_path, font_size)
        bbox = text_bbox(probe_draw, text, font)
        left, top, right, bottom = bbox
        text_w = right - left
        text_h = bottom - top
        if text_w <= max_w and text_h <= max_h:
            selected_font = font
            selected_bbox = bbox
            break

    if selected_font is not None and selected_bbox is not None:
        left, top, right, bottom = selected_bbox
        text_w = right - left
        text_h = bottom - top

        x = margin_x + (max_w - text_w) // 2 - left
        y = (height - text_h) // 2 - top

        draw = ImageDraw.Draw(canvas)
        draw.text((x, y), text, font=selected_font, fill=255)
        out = binarize_l_image(canvas, threshold=threshold, dither=False)
    else:
        # 文本过长时，先用 6 号字渲染，再压缩到 80x16 内。
        font = load_font(font_path, 6)
        bbox = text_bbox(probe_draw, text, font)
        left, top, right, bottom = bbox
        text_w = max(1, right - left)
        text_h = max(1, bottom - top)

        temp = Image.new("L", (text_w, text_h), 0)
        temp_draw = ImageDraw.Draw(temp)
        temp_draw.text((-left, -top), text, font=font, fill=255)

        scale = min(max_w / text_w, max_h / text_h)
        new_w = max(1, int(text_w * scale))
        new_h = max(1, int(text_h * scale))
        temp = temp.resize((new_w, new_h), Image.Resampling.LANCZOS)

        x = margin_x + (max_w - new_w) // 2
        y = (height - new_h) // 2
        canvas.paste(temp, (x, y))
        out = binarize_l_image(canvas, threshold=threshold, dither=False)

    if invert:
        out = ImageOps.invert(out)
        out = binarize_l_image(out, threshold=128, dither=False)
    return out


def render_avatar_bitmap(
    image_path: Path,
    size: tuple[int, int] = AVATAR_SIZE,
    threshold: int = 128,
    mode: Literal["cover", "contain", "stretch"] = "cover",
    dither: bool = False,
    invert: bool = False,
) -> Image.Image:
    """
    生成 48x64 头像位图。
    逻辑图像：255 为 bit=1，代表点亮像素。
    """
    if not image_path.exists():
        raise FileNotFoundError(f"头像图片不存在：{image_path}")

    img = Image.open(image_path)
    img = ImageOps.exif_transpose(img).convert("L")

    if mode == "cover":
        img = ImageOps.fit(img, size, method=Image.Resampling.LANCZOS, centering=(0.5, 0.5))
    elif mode == "contain":
        fitted = ImageOps.contain(img, size, method=Image.Resampling.LANCZOS)
        canvas = Image.new("L", size, 0)
        x = (size[0] - fitted.size[0]) // 2
        y = (size[1] - fitted.size[1]) // 2
        canvas.paste(fitted, (x, y))
        img = canvas
    elif mode == "stretch":
        img = img.resize(size, Image.Resampling.LANCZOS)
    else:
        raise ValueError(f"未知头像缩放模式：{mode}")

    out = binarize_l_image(img, threshold=threshold, dither=dither)

    if invert:
        out = ImageOps.invert(out)
        out = binarize_l_image(out, threshold=128, dither=False)

    return out


def assert_logical_bitmap(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    if img.size != size:
        raise ValueError(f"位图尺寸错误：got={img.size}, expected={size}")
    return img.convert("L")


def pack_bitmap(img: Image.Image, mode: PackMode) -> bytes:
    """
    将逻辑位图打包为字节数组。
    bit=1 表示像素点亮。
    """
    img = img.convert("L")
    width, height = img.size
    pixels = img.load()
    out = bytearray()

    if mode.startswith("row"):
        if width % 8 != 0:
            raise ValueError(f"row pack 要求宽度是 8 的倍数：width={width}")

        msb_first = mode == "row-msb"
        for y in range(height):
            for x0 in range(0, width, 8):
                value = 0
                for i in range(8):
                    x = x0 + i
                    on = pixels[x, y] >= 128
                    if on:
                        bit = 7 - i if msb_first else i
                        value |= 1 << bit
                out.append(value)

    elif mode.startswith("page"):
        if height % 8 != 0:
            raise ValueError(f"page pack 要求高度是 8 的倍数：height={height}")

        lsb_top = mode == "page-lsb"
        for page_y in range(0, height, 8):
            for x in range(width):
                value = 0
                for i in range(8):
                    y = page_y + i
                    on = pixels[x, y] >= 128
                    if on:
                        bit = i if lsb_top else 7 - i
                        value |= 1 << bit
                out.append(value)
    else:
        raise ValueError(f"未知 pack mode：{mode}")

    return bytes(out)


def unpack_bitmap(data: bytes, size: tuple[int, int], mode: PackMode) -> Image.Image:
    """
    用于 preview：按当前 pack mode 把 bytes 还原成图像。
    """
    width, height = size
    img = Image.new("L", size, 0)
    pixels = img.load()
    pos = 0

    if mode.startswith("row"):
        if width % 8 != 0:
            raise ValueError(f"row unpack 要求宽度是 8 的倍数：width={width}")

        msb_first = mode == "row-msb"
        expected = width * height // 8
        if len(data) != expected:
            raise ValueError(f"row unpack 数据长度错误：got={len(data)}, expected={expected}")

        for y in range(height):
            for x0 in range(0, width, 8):
                value = data[pos]
                pos += 1
                for i in range(8):
                    bit = 7 - i if msb_first else i
                    if value & (1 << bit):
                        pixels[x0 + i, y] = 255

    elif mode.startswith("page"):
        if height % 8 != 0:
            raise ValueError(f"page unpack 要求高度是 8 的倍数：height={height}")

        lsb_top = mode == "page-lsb"
        expected = width * height // 8
        if len(data) != expected:
            raise ValueError(f"page unpack 数据长度错误：got={len(data)}, expected={expected}")

        for page_y in range(0, height, 8):
            for x in range(width):
                value = data[pos]
                pos += 1
                for i in range(8):
                    bit = i if lsb_top else 7 - i
                    if value & (1 << bit):
                        pixels[x, page_y + i] = 255
    else:
        raise ValueError(f"未知 pack mode：{mode}")

    return img


def make_payload(
    avatar_img: Image.Image,
    name_img: Image.Image,
    dept_img: Image.Image,
    pack_mode: PackMode,
    invert_all_bits: bool = False,
) -> bytes:
    avatar_img = assert_logical_bitmap(avatar_img, AVATAR_SIZE)
    name_img = assert_logical_bitmap(name_img, TEXT_SIZE)
    dept_img = assert_logical_bitmap(dept_img, TEXT_SIZE)

    if invert_all_bits:
        avatar_img = binarize_l_image(ImageOps.invert(avatar_img), threshold=128)
        name_img = binarize_l_image(ImageOps.invert(name_img), threshold=128)
        dept_img = binarize_l_image(ImageOps.invert(dept_img), threshold=128)

    avatar_bytes = pack_bitmap(avatar_img, pack_mode)
    name_bytes = pack_bitmap(name_img, pack_mode)
    dept_bytes = pack_bitmap(dept_img, pack_mode)

    if len(avatar_bytes) != AVATAR_BYTES:
        raise AssertionError(f"avatar bytes={len(avatar_bytes)}, expected={AVATAR_BYTES}")
    if len(name_bytes) != TEXT_BYTES:
        raise AssertionError(f"name bytes={len(name_bytes)}, expected={TEXT_BYTES}")
    if len(dept_bytes) != TEXT_BYTES:
        raise AssertionError(f"dept bytes={len(dept_bytes)}, expected={TEXT_BYTES}")

    payload = avatar_bytes + name_bytes + dept_bytes
    if len(payload) != PAYLOAD_BYTES:
        raise AssertionError(f"payload bytes={len(payload)}, expected={PAYLOAD_BYTES}")

    return payload


def split_payload(payload: bytes) -> tuple[bytes, bytes, bytes]:
    if len(payload) != PAYLOAD_BYTES:
        raise ValueError(f"payload 长度错误：got={len(payload)}, expected={PAYLOAD_BYTES}")

    avatar = payload[0:AVATAR_BYTES]
    name = payload[AVATAR_BYTES:AVATAR_BYTES + TEXT_BYTES]
    dept = payload[AVATAR_BYTES + TEXT_BYTES:PAYLOAD_BYTES]
    return avatar, name, dept


def iter_payload_blocks(payload: bytes) -> Iterable[bytes]:
    if len(payload) != PAYLOAD_BYTES:
        raise ValueError(f"payload 长度错误：got={len(payload)}, expected={PAYLOAD_BYTES}")
    for i in range(0, len(payload), BLOCK_SIZE):
        yield payload[i:i + BLOCK_SIZE]


def save_preview(
    payload: bytes,
    path: Path,
    pack_mode: PackMode,
    scale: int = 4,
    show: bool = False,
) -> None:
    scale = max(1, min(16, int(scale)))
    avatar_bytes, name_bytes, dept_bytes = split_payload(payload)

    avatar_img = unpack_bitmap(avatar_bytes, AVATAR_SIZE, pack_mode)
    name_img = unpack_bitmap(name_bytes, TEXT_SIZE, pack_mode)
    dept_img = unpack_bitmap(dept_bytes, TEXT_SIZE, pack_mode)

    panels = [
        ("avatar 48x64", avatar_img),
        ("name 80x16", name_img),
        ("dept 80x16", dept_img),
    ]

    label_h = 14
    gap = 8

    panel_sizes = [(img.size[0] * scale, img.size[1] * scale) for _, img in panels]
    canvas_w = gap + sum(w for w, _ in panel_sizes) + gap * (len(panels) - 1) + gap
    canvas_h = gap + label_h + max(h for _, h in panel_sizes) + gap

    canvas = Image.new("RGB", (canvas_w, canvas_h), (0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default()

    x = gap
    for (label, img), (pw, ph) in zip(panels, panel_sizes):
        draw.text((x, gap), label, fill=(255, 255, 255), font=font)
        big = img.resize((pw, ph), Image.Resampling.NEAREST).convert("RGB")
        canvas.paste(big, (x, gap + label_h))
        x += pw + gap

    ensure_parent_dir(path)
    canvas.save(path)

    if show:
        canvas.show()


def dump_blocks_text(payload: bytes, path: Path) -> None:
    ensure_parent_dir(path)

    lines: list[str] = []
    lines.append("# NFC attendance image payload blocks")
    lines.append("# index, region, sector, block, hex16")
    lines.append("# header is NOT included; header target = sector 0 block 1")
    lines.append("")

    blocks = list(iter_payload_blocks(payload))
    if len(blocks) != len(CARD_IMAGE_BLOCK_MAP):
        raise AssertionError("payload block count and card block map count mismatch")

    for info, data in zip(CARD_IMAGE_BLOCK_MAP, blocks):
        lines.append(
            f"{info.index:02d},"
            f"{info.region},"
            f"sector={info.sector},"
            f"block={info.block},"
            f"hex={data.hex().upper()}"
        )

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")



def import_pyserial():
    """
    按需导入 pyserial。
    不使用 --send 时不会导入 serial，也不会要求安装 pyserial。
    """
    try:
        import serial  # type: ignore
    except ImportError as exc:
        raise RuntimeError("未安装 pyserial，请先执行：pip install pyserial") from exc
    return serial


def send_cmd(ser, cmd: str, verbose: bool = True) -> str:
    """
    发送一行命令并读取 MCU 单行响应。
    串口协议使用 ASCII 文本命令，以 \n 结尾。
    """
    if verbose:
        print(f">>> {cmd}")

    ser.write((cmd + "\n").encode("ascii"))
    ser.flush()

    raw = ser.readline()
    if not raw:
        resp = ""
    else:
        resp = raw.decode("utf-8", errors="replace").strip()

    if verbose:
        print(f"<<< {resp if resp else '<TIMEOUT>'}")

    return resp


def expect_response(resp: str, expected_prefix: str) -> bool:
    """
    判断 MCU 响应是否符合预期前缀。
    使用前缀匹配，兼容后续 MCU 在响应后追加调试信息。
    """
    return resp.startswith(expected_prefix)


def try_issue_cancel(ser) -> None:
    """
    出错时尽量复位 MCU 发卡状态机。
    这里不抛出异常，避免掩盖原始错误。
    """
    try:
        send_cmd(ser, "ISSUE_CANCEL", verbose=True)
    except Exception as exc:
        print(f"WARNING: 尝试 ISSUE_CANCEL 失败：{exc}", file=sys.stderr)


def send_payload_to_mcu(ser, payload: bytes, verbose: bool = False) -> None:
    """
    发送 ISSUE_IMAGE_BEGIN / 44 个 ISSUE_IMAGE_DATA / ISSUE_IMAGE_END。
    任意一步响应不符合预期都会抛出 RuntimeError。
    """
    if len(payload) != PAYLOAD_BYTES:
        raise ValueError(f"payload 长度错误：got={len(payload)}, expected={PAYLOAD_BYTES}")

    crc = crc16_xmodem(payload)
    cmd = f"ISSUE_IMAGE_BEGIN size={len(payload)} crc={crc:04X}"
    resp = send_cmd(ser, cmd, verbose=True)
    if not expect_response(resp, "OK IMAGE READY"):
        raise RuntimeError(f"ISSUE_IMAGE_BEGIN 失败，MCU 返回：{resp if resp else '<TIMEOUT>'}")

    for block_index, block in enumerate(iter_payload_blocks(payload)):
        block_hex = block.hex().upper()
        cmd = f"ISSUE_IMAGE_DATA block={block_index} hex={block_hex}"

        if verbose:
            resp = send_cmd(ser, cmd, verbose=True)
        else:
            print(f">>> ISSUE_IMAGE_DATA block={block_index:02d} hex=<16 bytes>")
            resp = send_cmd(ser, cmd, verbose=False)
            print(f"<<< {resp if resp else '<TIMEOUT>'}")

        expected = f"OK IMAGE DATA {block_index}"
        if not expect_response(resp, expected):
            raise RuntimeError(
                f"ISSUE_IMAGE_DATA block={block_index} 失败，"
                f"期望前缀：{expected}，MCU 返回：{resp if resp else '<TIMEOUT>'}"
            )

    resp = send_cmd(ser, "ISSUE_IMAGE_END", verbose=True)
    if expect_response(resp, "OK IMAGE END"):
        return

    if resp.startswith("ERR IMAGE CRC") or resp.startswith("ERR IMAGE INCOMPLETE"):
        raise RuntimeError(f"ISSUE_IMAGE_END 失败，MCU 返回：{resp}")

    raise RuntimeError(f"ISSUE_IMAGE_END 响应异常，MCU 返回：{resp if resp else '<TIMEOUT>'}")


def send_issue_sequence(
    payload: bytes,
    port: str,
    baud: int = 115200,
    timeout: float = 1.0,
    card_type: str = "image",
    card_id: str = "",
    commit: bool = False,
    verbose: bool = False,
) -> None:
    """
    完整串口发卡流程：
      PING -> ISSUE_CANCEL -> ISSUE_BEGIN -> payload -> ISSUE_IMAGE_END -> optional ISSUE_COMMIT
    """
    if not port:
        raise ValueError("--send 启用时必须指定 --port")
    if not card_id:
        raise ValueError("--send 启用时必须指定 --id")
    if card_type not in ("normal", "image", "admin"):
        raise ValueError(f"未知 card type：{card_type}")

    serial = import_pyserial()

    print(f"Opening serial port: {port}, baud={baud}, timeout={timeout}")
    ser = serial.Serial(port, baud, timeout=timeout)

    try:
        time.sleep(0.4)
        ser.reset_input_buffer()
        ser.reset_output_buffer()

        resp = send_cmd(ser, "PING", verbose=True)
        if not expect_response(resp, "PONG"):
            raise RuntimeError(f"PING 失败，MCU 返回：{resp if resp else '<TIMEOUT>'}")

        resp = send_cmd(ser, "ISSUE_CANCEL", verbose=True)
        if not expect_response(resp, "OK CANCEL"):
            raise RuntimeError(f"ISSUE_CANCEL 失败，MCU 返回：{resp if resp else '<TIMEOUT>'}")

        cmd = f"ISSUE_BEGIN type={card_type} id={card_id}"
        resp = send_cmd(ser, cmd, verbose=True)
        if not expect_response(resp, "OK ISSUE READY"):
            raise RuntimeError(f"ISSUE_BEGIN 失败，MCU 返回：{resp if resp else '<TIMEOUT>'}")

        try:
            send_payload_to_mcu(ser, payload, verbose=verbose)
        except Exception:
            try_issue_cancel(ser)
            raise

        if commit:
            resp = send_cmd(ser, "ISSUE_COMMIT", verbose=True)
            if expect_response(resp, "ERR ISSUE NOT_IMPL"):
                print("INFO: ISSUE_COMMIT 返回 ERR ISSUE NOT_IMPL，当前阶段视为预期占位响应。")
            elif resp.startswith("OK"):
                print("INFO: ISSUE_COMMIT 返回 OK。")
            else:
                raise RuntimeError(f"ISSUE_COMMIT 响应异常，MCU 返回：{resp if resp else '<TIMEOUT>'}")

        print("OK: serial issue sequence completed")

    except Exception:
        # 如果错误发生在 payload 之前，也尝试复位 MCU 状态机。
        try_issue_cancel(ser)
        raise
    finally:
        ser.close()
        print("Serial port closed")

def print_summary(
    payload: bytes,
    pack_mode: PackMode,
    font_path: Path | None,
    dump_bin: Path | None,
    preview: Path | None,
    dump_blocks: Path | None,
    print_blocks: bool,
) -> None:
    crc = crc16_xmodem(payload)
    blocks = list(iter_payload_blocks(payload))

    print("OK: payload generated")
    print(f"  payload size : {len(payload)} bytes")
    print(f"  block count  : {len(blocks)} blocks x 16 bytes")
    print(f"  layout       : avatar 384B + name 160B + dept 160B")
    print(f"  pack mode    : {pack_mode}")
    print(f"  bit meaning  : 1 = OLED pixel ON / foreground")
    print(f"  crc16-xmodem : 0x{crc:04X}")
    if font_path is not None:
        print(f"  font         : {font_path}")
    else:
        print("  font         : Pillow default font")
        print("  warning      : default font may not support Chinese; use --font <中文字体路径> if text is missing")
    if dump_bin is not None:
        print(f"  dump bin     : {dump_bin}")
    if preview is not None:
        print(f"  preview png  : {preview}")
    if dump_blocks is not None:
        print(f"  dump blocks  : {dump_blocks}")

    print("")
    print("Future serial command reference:")
    print(f"  ISSUE_IMAGE_BEGIN size={PAYLOAD_BYTES} crc={crc:04X}")
    print("  ISSUE_IMAGE_DATA block=0 hex=<32 hex chars>")
    print("  ...")
    print("  ISSUE_IMAGE_DATA block=43 hex=<32 hex chars>")
    print("  ISSUE_IMAGE_END")

    if print_blocks:
        print("")
        print("Payload blocks:")
        for info, data in zip(CARD_IMAGE_BLOCK_MAP, blocks):
            print(
                f"  block={info.index:02d} "
                f"region={info.region:<6} "
                f"target=S{info.sector:02d}B{info.block} "
                f"hex={data.hex().upper()}"
            )


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate 704-byte NFC attendance card image payload. Generate 704-byte NFC attendance card image payload and optionally send it to MCU over serial."
    )

    parser.add_argument("--avatar", required=True, help="头像图片路径，例如 ./avatar.jpg")
    parser.add_argument("--name", required=True, help="姓名文本，将渲染为 80x16 1bit 位图")
    parser.add_argument("--dept", required=True, help="部门文本，将渲染为 80x16 1bit 位图")

    parser.add_argument("--dump-bin", help="导出 704 字节 payload 到 bin 文件，例如 ./payload.bin")
    parser.add_argument(
        "--preview",
        nargs="?",
        const="payload_preview.png",
        help="导出预览 PNG；不填路径时默认 payload_preview.png",
    )
    parser.add_argument("--show-preview", action="store_true", help="生成 preview 后尝试用系统图片查看器打开")
    parser.add_argument("--dump-blocks", help="导出 44 个 16 字节块的文本清单，便于后续串口调试")
    parser.add_argument("--print-blocks", action="store_true", help="在终端打印 44 个 16 字节块 HEX")

    parser.add_argument(
        "--pack",
        choices=("page-lsb", "page-msb", "row-msb", "row-lsb"),
        default="page-lsb",
        help="1bit 位图打包格式，默认 page-lsb，常见于 SSD1306 OLED 页格式",
    )
    parser.add_argument("--font", help="指定 TTF/TTC/OTF 字体路径。中文姓名/部门建议指定或使用系统中文字体")
    parser.add_argument("--text-threshold", type=int, default=128, help="文本二值化阈值，默认 128")
    parser.add_argument("--avatar-threshold", type=int, default=128, help="头像二值化阈值，默认 128")
    parser.add_argument("--text-margin-x", type=int, default=0, help="姓名/部门左右留白像素，默认 0")
    parser.add_argument(
        "--avatar-mode",
        choices=("cover", "contain", "stretch"),
        default="cover",
        help="头像缩放模式：cover 裁剪填满；contain 保持完整留黑边；stretch 拉伸。默认 cover",
    )
    parser.add_argument("--dither-avatar", action="store_true", help="头像使用 Floyd-Steinberg 抖动二值化")
    parser.add_argument("--invert-avatar", action="store_true", help="头像反相")
    parser.add_argument("--invert-text", action="store_true", help="姓名和部门反相")
    parser.add_argument("--invert-all-bits", action="store_true", help="payload 全部位反相，用于适配 0=点亮 的显示函数")
    parser.add_argument("--preview-scale", type=int, default=4, help="preview 放大倍数，默认 4")

    parser.add_argument("--send", action="store_true", help="启用串口发送；不加该参数时只生成 payload/dump/preview，不打开串口")
    parser.add_argument("--port", help="串口号，例如 COM4；--send 启用时必填")
    parser.add_argument("--baud", type=int, default=115200, help="串口波特率，默认 115200")
    parser.add_argument("--timeout", type=float, default=1.0, help="串口响应超时时间，默认 1.0 秒")
    parser.add_argument(
        "--card-type",
        choices=("normal", "image", "admin"),
        default="image",
        help="卡类型：normal/image/admin，默认 image",
    )
    parser.add_argument("--id", dest="card_id", help="学号/工号，例如 10001；--send 启用时必填")
    parser.add_argument("--commit", action="store_true", help="ISSUE_IMAGE_END 成功后继续发送 ISSUE_COMMIT")
    parser.add_argument("--verbose", action="store_true", help="串口发送时打印完整 ISSUE_IMAGE_DATA 命令")

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    avatar_path = Path(args.avatar).expanduser()
    dump_bin_path = Path(args.dump_bin).expanduser() if args.dump_bin else None
    preview_path = Path(args.preview).expanduser() if args.preview else None
    dump_blocks_path = Path(args.dump_blocks).expanduser() if args.dump_blocks else None

    try:
        font_path = resolve_font_path(args.font)

        if font_path is None and (has_non_ascii(args.name) or has_non_ascii(args.dept)):
            print(
                "WARNING: 未找到中文字体，Pillow 默认字体可能无法显示中文。"
                "建议使用 --font 指定，例如 C:/Windows/Fonts/msyh.ttc",
                file=sys.stderr,
            )

        avatar_img = render_avatar_bitmap(
            avatar_path,
            size=AVATAR_SIZE,
            threshold=args.avatar_threshold,
            mode=args.avatar_mode,
            dither=args.dither_avatar,
            invert=args.invert_avatar,
        )
        name_img = render_text_bitmap(
            args.name,
            size=TEXT_SIZE,
            font_path=font_path,
            threshold=args.text_threshold,
            margin_x=args.text_margin_x,
            invert=args.invert_text,
        )
        dept_img = render_text_bitmap(
            args.dept,
            size=TEXT_SIZE,
            font_path=font_path,
            threshold=args.text_threshold,
            margin_x=args.text_margin_x,
            invert=args.invert_text,
        )

        payload = make_payload(
            avatar_img,
            name_img,
            dept_img,
            pack_mode=args.pack,
            invert_all_bits=args.invert_all_bits,
        )

        if dump_bin_path is not None:
            ensure_parent_dir(dump_bin_path)
            dump_bin_path.write_bytes(payload)

        if preview_path is not None:
            save_preview(
                payload,
                preview_path,
                pack_mode=args.pack,
                scale=args.preview_scale,
                show=args.show_preview,
            )

        if dump_blocks_path is not None:
            dump_blocks_text(payload, dump_blocks_path)

        print_summary(
            payload=payload,
            pack_mode=args.pack,
            font_path=font_path,
            dump_bin=dump_bin_path,
            preview=preview_path,
            dump_blocks=dump_blocks_path,
            print_blocks=args.print_blocks,
        )

        if args.send:
            print("")
            send_issue_sequence(
                payload=payload,
                port=args.port or "",
                baud=args.baud,
                timeout=args.timeout,
                card_type=args.card_type,
                card_id=args.card_id or "",
                commit=args.commit,
                verbose=args.verbose,
            )

        return 0

    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
