#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_tool/issue_card_gui.py

Tkinter GUI for NFC attendance issue_card.py.

设计目标：
- 不修改 issue_card.py
- 直接复用 issue_card.py 中已有的 payload 生成、预览、导出、串口发送函数
- 串口发送放在后台线程，避免 GUI 卡死

放置方式：
  pc_tool/
  ├─ issue_card.py
  └─ issue_card_gui.py

运行：
  python issue_card_gui.py

依赖：
  pip install pillow pyserial
"""

from __future__ import annotations

import contextlib
import io
import queue
import re
import threading
import time
import traceback
from dataclasses import dataclass
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from PIL import Image, ImageTk

import issue_card


class QueueWriter(io.TextIOBase):
    """把 print 输出转发到 Tk 主线程可轮询的 queue。"""

    def __init__(self, log_queue: queue.Queue[str]) -> None:
        super().__init__()
        self.log_queue = log_queue
        self._buf = ""

    def writable(self) -> bool:
        return True

    def write(self, s: str) -> int:
        if not s:
            return 0
        self._buf += s
        while "\n" in self._buf:
            line, self._buf = self._buf.split("\n", 1)
            self.log_queue.put(line + "\n")
        return len(s)

    def flush(self) -> None:
        if self._buf:
            self.log_queue.put(self._buf)
            self._buf = ""


@dataclass(frozen=True)
class CardReadResult:
    """CARD_READ 成功后的解析结果。"""

    uid: str
    card_id: str
    card_type: str
    status: str
    size: int
    crc: int
    payload: bytes


CARD_READ_ERROR_TEXT = {
    "BUSY": "MCU 当前正在发卡或执行其他卡操作",
    "NO_CARD": "10 秒内没有检测到卡",
    "AUTH": "M1 卡认证失败",
    "READ": "读块失败",
    "INVALID": "账户头 magic/checksum 不合法，不是本系统有效卡或已经销卡",
    "FAIL": "其他读卡错误",
}

CARD_CLEAR_ERROR_TEXT = {
    "BUSY": "MCU 当前正在发卡或执行其他卡操作",
    "NO_CARD": "10 秒内没有检测到卡",
    "AUTH": "M1 卡认证失败",
    "WRITE": "写块失败",
    "VERIFY": "写后读回校验失败",
    "FAIL": "其他清卡错误",
}


def explain_card_error(line: str, table: dict[str, str], action: str) -> str:
    """把 ERR CARD xxx 转成更容易理解的错误信息。"""
    m = re.fullmatch(r"ERR CARD\s+([A-Z_]+)", line.strip())
    if not m:
        return f"{action} 失败，MCU 返回：{line if line else '<TIMEOUT>'}"

    code = m.group(1)
    desc = table.get(code, "未知错误")
    return f"{action} 失败：{desc}（{line}）"


def parse_card_read_begin(line: str) -> dict[str, object]:
    """
    解析：
      OK CARD READ BEGIN uid=<8HEX> id=<workerId> type=<normal|image|admin> status=<status> size=704 crc=<4HEX>
    """
    line = line.strip()
    prefix = "OK CARD READ BEGIN "
    if not line.startswith(prefix):
        raise ValueError(f"CARD_READ BEGIN 格式错误：{line if line else '<TIMEOUT>'}")

    kv = dict(re.findall(r"(\w+)=([^\s]+)", line[len(prefix):]))
    missing = [key for key in ("uid", "id", "type", "status", "size", "crc") if key not in kv]
    if missing:
        raise ValueError(f"CARD_READ BEGIN 缺少字段：{', '.join(missing)}；原始行：{line}")

    uid = kv["uid"].upper()
    if not re.fullmatch(r"[0-9A-F]{8}", uid):
        raise ValueError(f"CARD_READ BEGIN uid 格式错误：{kv['uid']}")

    card_type = kv["type"]
    if card_type not in ("normal", "image", "admin"):
        raise ValueError(f"CARD_READ BEGIN type 非法：{card_type}")

    try:
        size = int(kv["size"], 10)
    except ValueError as exc:
        raise ValueError(f"CARD_READ BEGIN size 非法：{kv['size']}") from exc
    if size != issue_card.PAYLOAD_BYTES:
        raise ValueError(f"CARD_READ BEGIN size 错误：got={size}, expected={issue_card.PAYLOAD_BYTES}")

    crc_text = kv["crc"].upper()
    if not re.fullmatch(r"[0-9A-F]{4}", crc_text):
        raise ValueError(f"CARD_READ BEGIN crc 格式错误：{kv['crc']}")
    crc = int(crc_text, 16)

    return {
        "uid": uid,
        "card_id": kv["id"],
        "card_type": card_type,
        "status": kv["status"],
        "size": size,
        "crc": crc,
    }


def parse_card_read_data(line: str) -> tuple[int, bytes]:
    """
    解析：
      OK CARD READ DATA block=00 hex=<32HEX>
    block 按协议视为十进制 00~43。
    """
    line = line.strip()
    m = re.fullmatch(r"OK CARD READ DATA\s+block=([0-9]{2})\s+hex=([0-9A-Fa-f]+)", line)
    if not m:
        raise ValueError(f"CARD_READ DATA 格式错误：{line if line else '<TIMEOUT>'}")

    block_index = int(m.group(1), 10)
    hex_text = m.group(2)
    if len(hex_text) != issue_card.BLOCK_SIZE * 2:
        raise ValueError(
            f"CARD_READ DATA block={block_index:02d} HEX 长度错误："
            f"got={len(hex_text)}, expected={issue_card.BLOCK_SIZE * 2}"
        )

    try:
        data = bytes.fromhex(hex_text)
    except ValueError as exc:
        raise ValueError(f"CARD_READ DATA block={block_index:02d} HEX 内容非法") from exc

    if len(data) != issue_card.BLOCK_SIZE:
        raise ValueError(
            f"CARD_READ DATA block={block_index:02d} 数据长度错误："
            f"got={len(data)}, expected={issue_card.BLOCK_SIZE}"
        )

    return block_index, data


def _mask_card_read_data_for_log(line: str, verbose: bool) -> str:
    if verbose:
        return line
    if line.startswith("OK CARD READ DATA "):
        return re.sub(r"hex=[0-9A-Fa-f]+", "hex=<16 bytes>", line)
    return line


def read_mcu_line(ser, verbose: bool = False) -> str:
    """读取 MCU 一行响应并按 GUI 日志风格打印。"""
    raw = ser.readline()
    if not raw:
        line = ""
    else:
        line = raw.decode("utf-8", errors="replace").strip()
    print(f"<<< {_mask_card_read_data_for_log(line, verbose) if line else '<TIMEOUT>'}")
    return line


def _open_mcu_serial(port: str, baud: int, timeout: float, action: str):
    if not port:
        raise ValueError("请输入串口号，例如 COM4")

    serial = issue_card.import_pyserial()
    op_timeout = max(float(timeout), 20.0)
    if op_timeout != float(timeout):
        print(f"INFO: {action} 需要等待放卡，串口 timeout 临时使用 {op_timeout:.1f}s")

    print(f"Opening serial port: {port}, baud={baud}, timeout={op_timeout}")
    ser = serial.Serial(port, int(baud), timeout=op_timeout)
    time.sleep(0.4)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def read_card_from_mcu(port: str, baud: int = 115200, timeout: float = 20.0, verbose: bool = False) -> CardReadResult:
    """
    执行 CARD_READ：PING -> CARD_READ -> BEGIN -> 44 DATA -> END -> CRC 校验。
    成功返回 CardReadResult；失败抛出异常，由 GUI worker 显示到日志和弹窗。
    """
    ser = _open_mcu_serial(port, baud, timeout, "CARD_READ")
    try:
        resp = issue_card.send_cmd(ser, "PING", verbose=True)
        if not issue_card.expect_response(resp, "PONG"):
            raise RuntimeError(f"PING 失败，MCU 返回：{resp if resp else '<TIMEOUT>'}")

        first = issue_card.send_cmd(ser, "CARD_READ", verbose=True)
        if first.startswith("ERR CARD "):
            raise RuntimeError(explain_card_error(first, CARD_READ_ERROR_TEXT, "CARD_READ"))
        info = parse_card_read_begin(first)

        payload_buf = bytearray()
        for expected_block in range(issue_card.PAYLOAD_BLOCKS):
            line = read_mcu_line(ser, verbose=verbose)
            if not line:
                raise TimeoutError(f"CARD_READ 超时：等待 DATA block={expected_block:02d} 无响应")
            if line.startswith("ERR CARD "):
                raise RuntimeError(explain_card_error(line, CARD_READ_ERROR_TEXT, "CARD_READ"))

            block_index, block_data = parse_card_read_data(line)
            if block_index != expected_block:
                raise ValueError(f"CARD_READ DATA block 顺序错误：got={block_index:02d}, expected={expected_block:02d}")
            payload_buf.extend(block_data)

        end_line = read_mcu_line(ser, verbose=verbose)
        if not end_line:
            raise TimeoutError("CARD_READ 超时：等待 OK CARD READ END 无响应")
        if end_line.startswith("ERR CARD "):
            raise RuntimeError(explain_card_error(end_line, CARD_READ_ERROR_TEXT, "CARD_READ"))
        if end_line != "OK CARD READ END":
            raise ValueError(f"CARD_READ END 格式错误：{end_line}")

        payload = bytes(payload_buf)
        if len(payload) != issue_card.PAYLOAD_BYTES:
            raise ValueError(f"payload 长度错误：got={len(payload)}, expected={issue_card.PAYLOAD_BYTES}")

        local_crc = issue_card.crc16_xmodem(payload)
        mcu_crc = int(info["crc"])
        if local_crc != mcu_crc:
            raise ValueError(f"CRC 校验失败：本地=0x{local_crc:04X}, MCU=0x{mcu_crc:04X}")

        print(
            "OK: CARD_READ CRC 校验通过，"
            f"uid={info['uid']} id={info['card_id']} type={info['card_type']} "
            f"size={len(payload)} crc=0x{local_crc:04X}"
        )

        return CardReadResult(
            uid=str(info["uid"]),
            card_id=str(info["card_id"]),
            card_type=str(info["card_type"]),
            status=str(info["status"]),
            size=int(info["size"]),
            crc=mcu_crc,
            payload=payload,
        )
    finally:
        ser.close()
        print("Serial port closed")


def clear_card_on_mcu(port: str, baud: int = 115200, timeout: float = 20.0) -> None:
    """执行 CARD_CLEAR：PING -> CARD_CLEAR。"""
    ser = _open_mcu_serial(port, baud, timeout, "CARD_CLEAR")
    try:
        resp = issue_card.send_cmd(ser, "PING", verbose=True)
        if not issue_card.expect_response(resp, "PONG"):
            raise RuntimeError(f"PING 失败，MCU 返回：{resp if resp else '<TIMEOUT>'}")

        resp = issue_card.send_cmd(ser, "CARD_CLEAR", verbose=True)
        if resp.startswith("ERR CARD "):
            raise RuntimeError(explain_card_error(resp, CARD_CLEAR_ERROR_TEXT, "CARD_CLEAR"))
        if not issue_card.expect_response(resp, "OK CARD CLEAR DONE"):
            raise RuntimeError(f"CARD_CLEAR 响应异常，MCU 返回：{resp if resp else '<TIMEOUT>'}")

        print("OK: CARD_CLEAR 清卡成功")
    finally:
        ser.close()
        print("Serial port closed")


def payload_to_preview_image(payload: bytes, pack_mode: str, scale: int = 4) -> Image.Image:
    """把 704 字节 payload 还原为头像、姓名、部门三段预览图。"""
    scale = max(1, min(16, int(scale)))
    avatar_bytes, name_bytes, dept_bytes = issue_card.split_payload(payload)
    avatar_img = issue_card.unpack_bitmap(avatar_bytes, issue_card.AVATAR_SIZE, pack_mode)
    name_img = issue_card.unpack_bitmap(name_bytes, issue_card.TEXT_SIZE, pack_mode)
    dept_img = issue_card.unpack_bitmap(dept_bytes, issue_card.TEXT_SIZE, pack_mode)

    panels = [
        ("avatar 48x64", avatar_img),
        ("name 80x16", name_img),
        ("dept 80x16", dept_img),
    ]
    label_h = 18
    gap = 10
    panel_sizes = [(img.size[0] * scale, img.size[1] * scale) for _, img in panels]
    canvas_w = gap + sum(w for w, _ in panel_sizes) + gap * (len(panels) - 1) + gap
    canvas_h = gap + label_h + max(h for _, h in panel_sizes) + gap

    canvas = Image.new("RGB", (canvas_w, canvas_h), (0, 0, 0))
    from PIL import ImageDraw, ImageFont

    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default()
    x = gap
    for (label, img), (pw, ph) in zip(panels, panel_sizes):
        draw.text((x, gap), label, fill=(255, 255, 255), font=font)
        big = img.resize((pw, ph), Image.Resampling.NEAREST).convert("RGB")
        canvas.paste(big, (x, gap + label_h))
        x += pw + gap
    return canvas


def translate_list_record_line(line: str) -> str | None:
    """
    将下位机 LIST 的 ASCII 记录行转译成验收要求中的中文说明。

    支持格式示例：
      OK LIST 2026-07-06 12:34:56 DEV=1 ID=10001 IN
      OK LIST 2026-07-06 12:36:20 DEV=1 ID=10001 OUT DUR=84s

    返回：
      2026年07月06日 12:34:56 于 编号1考勤机 处 签到，卡号 10001
    """
    line = line.strip()
    m = re.fullmatch(
        r"OK LIST\s+"
        r"(\d{4})-(\d{2})-(\d{2})\s+"
        r"(\d{2}):(\d{2}):(\d{2})\s+"
        r"DEV=([^\s]+)\s+"
        r"ID=([^\s]+)\s+"
        r"(IN|OUT|DENY|UNK)"
        r"(?:\s+DUR=(\d+)s)?"
        r"(?:\s+.*)?",
        line,
    )
    if not m:
        return None

    year, month, day, hh, mm, ss, dev, card_id, event, dur_s = m.groups()
    event_text = {
        "IN": "签到",
        "OUT": "离开",
        "DENY": "拒绝",
        "UNK": "未知",
    }.get(event, event)

    result = f"{year}年{month}月{day}日 {hh}:{mm}:{ss} 于 编号{dev}考勤机 处 {event_text}，卡号 {card_id}"

    if dur_s is not None and event == "OUT":
        try:
            dur = int(dur_s)
            hours = dur // 3600
            minutes = (dur % 3600) // 60
            seconds = dur % 60
            if hours > 0 or minutes > 0:
                result += f"，到场时长 {hours}小时{minutes}分钟"
            else:
                result += f"，到场时长 {seconds}秒"
        except ValueError:
            result += f"，到场时长 {dur_s}秒"

    return result


def is_list_boundary_line(line: str) -> bool:
    """判断是否是 LIST 查询的开始/结束行。"""
    line = line.strip()
    return line.startswith("OK LIST BEGIN") or line.startswith("OK LIST END")


class IssueCardGUI(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("NFC考勤系统上位机 - 专业综合实践设计 II")
        self.geometry("1250x900")
        self.minsize(1020, 720)

        self.log_queue: queue.Queue[str] = queue.Queue()
        self.history_queue: queue.Queue[str] = queue.Queue()
        self.worker: threading.Thread | None = None
        self.last_payload: bytes | None = None
        self.last_read_payload: bytes | None = None
        self.last_preview_image: Image.Image | None = None
        self.preview_photo: ImageTk.PhotoImage | None = None

        # 手动串口通信区使用的连接对象。
        # 发卡/读卡/清卡仍复用原有流程独立打开串口，开始这些操作前会自动关闭手动串口连接。
        self.serial_conn = None
        self.rx_thread: threading.Thread | None = None
        self.rx_stop_event = threading.Event()
        self.port_display_map: dict[str, str] = {}
        self._active_scroll_canvas: tk.Canvas | None = None

        self._build_vars()
        self._build_ui()
        self.refresh_serial_ports()
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(50, self._poll_log_queue)

    def _build_vars(self) -> None:
        self.avatar_var = tk.StringVar()
        self.name_var = tk.StringVar(value="张三")
        self.dept_var = tk.StringVar(value="电子信息")
        self.card_id_var = tk.StringVar(value="10001")
        self.card_type_var = tk.StringVar(value="image")

        self.font_var = tk.StringVar(value=str(Path(r"C:\Windows\Fonts\msyh.ttc")))
        self.pack_var = tk.StringVar(value="row-msb")
        self.avatar_mode_var = tk.StringVar(value="cover")
        self.avatar_threshold_var = tk.IntVar(value=128)
        self.text_threshold_var = tk.IntVar(value=128)
        self.text_margin_x_var = tk.IntVar(value=0)
        self.preview_scale_var = tk.IntVar(value=4)

        self.dither_avatar_var = tk.BooleanVar(value=False)
        self.invert_avatar_var = tk.BooleanVar(value=False)
        self.invert_text_var = tk.BooleanVar(value=False)
        self.invert_all_bits_var = tk.BooleanVar(value=False)

        self.port_var = tk.StringVar(value="")
        self.baud_var = tk.IntVar(value=115200)
        self.timeout_var = tk.DoubleVar(value=10.0)
        self.commit_var = tk.BooleanVar(value=False)
        self.verbose_var = tk.BooleanVar(value=False)
        self.serial_status_var = tk.StringVar(value="串口未连接")
        self.manual_cmd_var = tk.StringVar(value="PING")

        self.status_var = tk.StringVar(value="空闲")
        self.summary_var = tk.StringVar(value="未生成 payload")

        self.read_uid_var = tk.StringVar(value="—")
        self.read_id_var = tk.StringVar(value="—")
        self.read_type_var = tk.StringVar(value="—")
        self.read_status_var = tk.StringVar(value="—")
        self.read_size_var = tk.StringVar(value="—")
        self.read_crc_var = tk.StringVar(value="—")
        self.read_note_var = tk.StringVar(value="尚未读取卡片")

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=10)
        root.pack(fill=tk.BOTH, expand=True)
        root.columnconfigure(0, weight=0)
        root.columnconfigure(1, weight=1)
        root.rowconfigure(0, weight=0)
        root.rowconfigure(1, weight=1)
        root.rowconfigure(2, weight=0)

        self._build_header_frame(root)

        # 左侧选项区改成可滚动，避免小屏幕或高 DPI 缩放时按钮被裁掉。
        left_outer = ttk.Frame(root)
        left_outer.grid(row=1, column=0, sticky="nsw", padx=(0, 10))
        left_outer.rowconfigure(0, weight=1)
        left_outer.columnconfigure(0, weight=1)

        self.left_canvas = tk.Canvas(left_outer, width=410, highlightthickness=0)
        self.left_canvas.grid(row=0, column=0, sticky="ns")

        left_scrollbar = ttk.Scrollbar(left_outer, orient="vertical", command=self.left_canvas.yview)
        left_scrollbar.grid(row=0, column=1, sticky="ns")
        self.left_canvas.configure(yscrollcommand=left_scrollbar.set)

        left = ttk.Frame(self.left_canvas)
        self.left_window = self.left_canvas.create_window((0, 0), window=left, anchor="nw")

        def _update_left_scroll_region(_event: tk.Event | None = None) -> None:
            self.left_canvas.configure(scrollregion=self.left_canvas.bbox("all"))

        def _sync_left_width(event: tk.Event) -> None:
            # 保持内部 Frame 宽度和 Canvas 一致，Entry 才能正常横向填充。
            self.left_canvas.itemconfigure(self.left_window, width=event.width)

        left.bind("<Configure>", _update_left_scroll_region)
        self.left_canvas.bind("<Configure>", _sync_left_width)

        # 右侧区域也放入 Canvas，避免读卡后预览/读卡结果/历史记录内容变高时，
        # 把通信日志和底部状态栏挤出窗口。
        right_outer = ttk.Frame(root)
        right_outer.grid(row=1, column=1, sticky="nsew")
        right_outer.rowconfigure(0, weight=1)
        right_outer.columnconfigure(0, weight=1)

        self.right_canvas = tk.Canvas(right_outer, highlightthickness=0)
        self.right_canvas.grid(row=0, column=0, sticky="nsew")

        right_scrollbar = ttk.Scrollbar(right_outer, orient="vertical", command=self.right_canvas.yview)
        right_scrollbar.grid(row=0, column=1, sticky="ns")
        self.right_canvas.configure(yscrollcommand=right_scrollbar.set)

        right = ttk.Frame(self.right_canvas)
        self.right_window = self.right_canvas.create_window((0, 0), window=right, anchor="nw")

        def _update_right_scroll_region(_event: tk.Event | None = None) -> None:
            self.right_canvas.configure(scrollregion=self.right_canvas.bbox("all"))

        def _sync_right_width(event: tk.Event) -> None:
            # 保持右侧内部 Frame 宽度和 Canvas 一致，避免 LabelFrame 横向撑开窗口。
            self.right_canvas.itemconfigure(self.right_window, width=event.width)

        right.bind("<Configure>", _update_right_scroll_region)
        self.right_canvas.bind("<Configure>", _sync_right_width)

        right.columnconfigure(0, weight=1)
        right.rowconfigure(0, weight=0)
        right.rowconfigure(1, weight=0)
        right.rowconfigure(2, weight=0)
        right.rowconfigure(3, weight=1)

        self._build_card_frame(left)
        self._build_bitmap_frame(left)
        self._build_serial_frame(left)
        self._build_action_frame(left)

        self._build_preview_frame(right)
        self._build_read_result_frame(right)
        self._build_history_frame(right)
        self._build_log_frame(right)

        # 左右两栏都支持鼠标滚轮滚动。使用 active canvas，避免左侧 bind_all
        # 抢走右侧滚轮事件。
        self._bind_mousewheel_area(left_outer, self.left_canvas)
        self._bind_mousewheel_area(right_outer, self.right_canvas)
        self.bind_all("<MouseWheel>", self._on_global_mousewheel, add="+")
        self.bind_all("<Button-4>", self._on_global_button4, add="+")
        self.bind_all("<Button-5>", self._on_global_button5, add="+")

        self._build_status_bar(root)

    def _bind_mousewheel_area(self, widget: tk.Widget, canvas: tk.Canvas) -> None:
        """让某个区域内的所有子控件激活对应 Canvas 的鼠标滚轮滚动。"""
        widget.bind("<Enter>", lambda _event, c=canvas: self._set_active_scroll_canvas(c), add="+")
        for child in widget.winfo_children():
            self._bind_mousewheel_area(child, canvas)

    def _set_active_scroll_canvas(self, canvas: tk.Canvas) -> None:
        self._active_scroll_canvas = canvas

    def _scroll_active_canvas(self, units: int) -> str | None:
        canvas = self._active_scroll_canvas
        if canvas is None:
            return None
        try:
            canvas.yview_scroll(units, "units")
            return "break"
        except tk.TclError:
            return None

    def _on_global_mousewheel(self, event: tk.Event) -> str | None:
        if not event.delta:
            return None
        return self._scroll_active_canvas(int(-event.delta / 120))

    def _on_global_button4(self, _event: tk.Event) -> str | None:
        return self._scroll_active_canvas(-1)

    def _on_global_button5(self, _event: tk.Event) -> str | None:
        return self._scroll_active_canvas(1)

    def _build_header_frame(self, parent: ttk.Frame) -> None:
        """验收展示用顶部大标题：课程、项目、成员信息。"""
        frm = ttk.Frame(parent, padding=(0, 0, 0, 10))
        frm.grid(row=0, column=0, columnspan=2, sticky="ew")
        frm.columnconfigure(0, weight=1)

        title_font = ("Microsoft YaHei", 18, "bold")
        sub_font = ("Microsoft YaHei", 13, "bold")

        ttk.Label(
            frm,
            text="专业综合实践设计 II",
            anchor="center",
            font=title_font,
        ).grid(row=0, column=0, sticky="ew")

        ttk.Label(
            frm,
            text="NFC考勤系统",
            anchor="center",
            font=sub_font,
        ).grid(row=1, column=0, sticky="ew", pady=(3, 0))

    def _build_card_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="卡片信息", padding=8)
        frm.pack(fill=tk.X, pady=(0, 8))
        frm.columnconfigure(1, weight=1)

        ttk.Label(frm, text="头像").grid(row=0, column=0, sticky="w", pady=3)
        ttk.Entry(frm, textvariable=self.avatar_var, width=34).grid(row=0, column=1, sticky="ew", pady=3)
        ttk.Button(frm, text="选择", command=self.choose_avatar).grid(row=0, column=2, padx=(5, 0), pady=3)

        ttk.Label(frm, text="姓名").grid(row=1, column=0, sticky="w", pady=3)
        ttk.Entry(frm, textvariable=self.name_var).grid(row=1, column=1, columnspan=2, sticky="ew", pady=3)

        ttk.Label(frm, text="部门").grid(row=2, column=0, sticky="w", pady=3)
        ttk.Entry(frm, textvariable=self.dept_var).grid(row=2, column=1, columnspan=2, sticky="ew", pady=3)

        ttk.Label(frm, text="学号/工号").grid(row=3, column=0, sticky="w", pady=3)
        ttk.Entry(frm, textvariable=self.card_id_var).grid(row=3, column=1, columnspan=2, sticky="ew", pady=3)

        ttk.Label(frm, text="卡类型").grid(row=4, column=0, sticky="w", pady=3)
        ttk.Combobox(
            frm,
            textvariable=self.card_type_var,
            values=("normal", "image", "admin"),
            state="readonly",
            width=12,
        ).grid(row=4, column=1, sticky="w", pady=3)

    def _build_bitmap_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="位图参数", padding=8)
        frm.pack(fill=tk.X, pady=(0, 8))
        frm.columnconfigure(1, weight=1)

        ttk.Label(frm, text="字体").grid(row=0, column=0, sticky="w", pady=3)
        ttk.Entry(frm, textvariable=self.font_var, width=34).grid(row=0, column=1, sticky="ew", pady=3)
        ttk.Button(frm, text="选择", command=self.choose_font).grid(row=0, column=2, padx=(5, 0), pady=3)

        ttk.Label(frm, text="pack").grid(row=1, column=0, sticky="w", pady=3)
        ttk.Combobox(
            frm,
            textvariable=self.pack_var,
            values=("page-lsb", "page-msb", "row-msb", "row-lsb"),
            state="readonly",
            width=12,
        ).grid(row=1, column=1, sticky="w", pady=3)

        ttk.Label(frm, text="头像模式").grid(row=2, column=0, sticky="w", pady=3)
        ttk.Combobox(
            frm,
            textvariable=self.avatar_mode_var,
            values=("cover", "contain", "stretch"),
            state="readonly",
            width=12,
        ).grid(row=2, column=1, sticky="w", pady=3)

        ttk.Label(frm, text="头像阈值").grid(row=3, column=0, sticky="w", pady=3)
        ttk.Spinbox(frm, from_=0, to=255, textvariable=self.avatar_threshold_var, width=8).grid(row=3, column=1, sticky="w", pady=3)

        ttk.Label(frm, text="文本阈值").grid(row=4, column=0, sticky="w", pady=3)
        ttk.Spinbox(frm, from_=0, to=255, textvariable=self.text_threshold_var, width=8).grid(row=4, column=1, sticky="w", pady=3)

        ttk.Label(frm, text="文本留白").grid(row=5, column=0, sticky="w", pady=3)
        ttk.Spinbox(frm, from_=0, to=39, textvariable=self.text_margin_x_var, width=8).grid(row=5, column=1, sticky="w", pady=3)

        ttk.Checkbutton(frm, text="头像抖动", variable=self.dither_avatar_var).grid(row=6, column=0, sticky="w", pady=2)
        ttk.Checkbutton(frm, text="头像反相", variable=self.invert_avatar_var).grid(row=6, column=1, sticky="w", pady=2)
        ttk.Checkbutton(frm, text="文本反相", variable=self.invert_text_var).grid(row=7, column=0, sticky="w", pady=2)
        ttk.Checkbutton(frm, text="payload 全部反相", variable=self.invert_all_bits_var).grid(row=7, column=1, sticky="w", pady=2)

    def _build_serial_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="串口通信", padding=8)
        frm.pack(fill=tk.X, pady=(0, 8))
        frm.columnconfigure(1, weight=1)

        ttk.Label(frm, text="串口").grid(row=0, column=0, sticky="w", pady=3)
        self.port_combo = ttk.Combobox(frm, textvariable=self.port_var, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="ew", pady=3)
        ttk.Button(frm, text="刷新", command=self.refresh_serial_ports).grid(row=0, column=2, padx=(5, 0), pady=3)

        ttk.Label(frm, text="波特率").grid(row=1, column=0, sticky="w", pady=3)
        self.baud_combo = ttk.Combobox(
            frm,
            textvariable=self.baud_var,
            values=(9600, 19200, 38400, 57600, 115200, 230400),
            state="readonly",
            width=12,
        )
        self.baud_combo.grid(row=1, column=1, sticky="w", pady=3)

        ttk.Label(frm, text="超时(s)").grid(row=2, column=0, sticky="w", pady=3)
        ttk.Entry(frm, textvariable=self.timeout_var, width=12).grid(row=2, column=1, sticky="w", pady=3)

        btn_row = ttk.Frame(frm)
        btn_row.grid(row=3, column=0, columnspan=3, sticky="ew", pady=(4, 2))
        btn_row.columnconfigure(0, weight=1)
        btn_row.columnconfigure(1, weight=1)
        self.open_serial_btn = ttk.Button(btn_row, text="打开串口", command=self.open_serial_port)
        self.open_serial_btn.grid(row=0, column=0, sticky="ew", padx=(0, 4))
        self.close_serial_btn = ttk.Button(btn_row, text="关闭串口", command=self.close_serial_port, state=tk.DISABLED)
        self.close_serial_btn.grid(row=0, column=1, sticky="ew", padx=(4, 0))

        ttk.Label(frm, textvariable=self.serial_status_var).grid(row=4, column=0, columnspan=3, sticky="w", pady=2)

        send_row = ttk.Frame(frm)
        send_row.grid(row=5, column=0, columnspan=3, sticky="ew", pady=(4, 2))
        send_row.columnconfigure(0, weight=1)
        self.manual_cmd_entry = ttk.Entry(send_row, textvariable=self.manual_cmd_var)
        self.manual_cmd_entry.grid(row=0, column=0, sticky="ew", padx=(0, 5))
        self.manual_cmd_entry.bind("<Return>", lambda _event: self.send_manual_command())
        self.manual_send_btn = ttk.Button(send_row, text="发送", command=self.send_manual_command)
        self.manual_send_btn.grid(row=0, column=1)

        ttk.Checkbutton(frm, text="发送 ISSUE_COMMIT", variable=self.commit_var).grid(row=6, column=0, columnspan=3, sticky="w", pady=2)
        ttk.Checkbutton(frm, text="verbose 完整打印 DATA", variable=self.verbose_var).grid(row=7, column=0, columnspan=3, sticky="w", pady=2)

    def _build_action_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="操作", padding=8)
        frm.pack(fill=tk.X)

        self.preview_btn = ttk.Button(frm, text="生成预览", command=self.on_preview)
        self.preview_btn.pack(fill=tk.X, pady=3)

        self.dump_bin_btn = ttk.Button(frm, text="导出 payload.bin", command=self.on_dump_bin)
        self.dump_bin_btn.pack(fill=tk.X, pady=3)

        self.dump_blocks_btn = ttk.Button(frm, text="导出 blocks.txt", command=self.on_dump_blocks)
        self.dump_blocks_btn.pack(fill=tk.X, pady=3)

        self.send_btn = ttk.Button(frm, text="发送到 MCU", command=self.on_send)
        self.send_btn.pack(fill=tk.X, pady=3)

        self.read_btn = ttk.Button(frm, text="读取卡片 CARD_READ", command=self.on_read_card)
        self.read_btn.pack(fill=tk.X, pady=3)

        self.clear_card_btn = ttk.Button(frm, text="销卡 / 清卡 CARD_CLEAR", command=self.on_clear_card)
        self.clear_card_btn.pack(fill=tk.X, pady=3)

        self.list_btn = ttk.Button(frm, text="查询历史记录 LIST", command=self.query_history_records)
        self.list_btn.pack(fill=tk.X, pady=3)

        ttk.Button(frm, text="清空日志", command=self.clear_log).pack(fill=tk.X, pady=3)

    def _build_preview_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="预览", padding=8)
        frm.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        frm.columnconfigure(0, weight=1)

        self.preview_label = ttk.Label(frm, text="未生成预览", anchor="center")
        self.preview_label.grid(row=0, column=0, sticky="ew")

        ttk.Label(frm, textvariable=self.summary_var).grid(row=1, column=0, sticky="w", pady=(8, 0))

    def _build_read_result_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="读卡结果", padding=8)
        frm.grid(row=1, column=0, sticky="ew", pady=(0, 8))
        frm.columnconfigure(1, weight=1)
        frm.columnconfigure(3, weight=1)

        ttk.Label(frm, text="UID").grid(row=0, column=0, sticky="w", padx=(0, 6), pady=2)
        ttk.Label(frm, textvariable=self.read_uid_var).grid(row=0, column=1, sticky="w", pady=2)
        ttk.Label(frm, text="ID").grid(row=0, column=2, sticky="w", padx=(12, 6), pady=2)
        ttk.Label(frm, textvariable=self.read_id_var).grid(row=0, column=3, sticky="w", pady=2)

        ttk.Label(frm, text="type").grid(row=1, column=0, sticky="w", padx=(0, 6), pady=2)
        ttk.Label(frm, textvariable=self.read_type_var).grid(row=1, column=1, sticky="w", pady=2)
        ttk.Label(frm, text="status").grid(row=1, column=2, sticky="w", padx=(12, 6), pady=2)
        ttk.Label(frm, textvariable=self.read_status_var).grid(row=1, column=3, sticky="w", pady=2)

        ttk.Label(frm, text="size").grid(row=2, column=0, sticky="w", padx=(0, 6), pady=2)
        ttk.Label(frm, textvariable=self.read_size_var).grid(row=2, column=1, sticky="w", pady=2)
        ttk.Label(frm, text="CRC").grid(row=2, column=2, sticky="w", padx=(12, 6), pady=2)
        ttk.Label(frm, textvariable=self.read_crc_var).grid(row=2, column=3, sticky="w", pady=2)

        ttk.Label(frm, textvariable=self.read_note_var, wraplength=620).grid(row=3, column=0, columnspan=4, sticky="w", pady=(6, 2))

        self.save_read_payload_btn = ttk.Button(
            frm,
            text="保存读出 payload.bin",
            command=self.on_save_read_payload,
            state=tk.DISABLED,
        )
        self.save_read_payload_btn.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(4, 0))


    def _build_history_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="历史刷卡记录（LIST 转译）", padding=8)
        frm.grid(row=2, column=0, sticky="ew", pady=(0, 8))
        frm.columnconfigure(0, weight=1)
        frm.rowconfigure(0, weight=1)

        self.history_text = tk.Text(frm, height=5, wrap="word")
        self.history_text.grid(row=0, column=0, sticky="nsew")

        ybar = ttk.Scrollbar(frm, orient="vertical", command=self.history_text.yview)
        ybar.grid(row=0, column=1, sticky="ns")
        self.history_text.configure(yscrollcommand=ybar.set)

        btn_row = ttk.Frame(frm)
        btn_row.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        btn_row.columnconfigure(0, weight=1)
        btn_row.columnconfigure(1, weight=1)
        ttk.Button(btn_row, text="查询 LIST", command=self.query_history_records).grid(row=0, column=0, sticky="ew", padx=(0, 4))
        ttk.Button(btn_row, text="清空历史显示", command=self.clear_history_records).grid(row=0, column=1, sticky="ew", padx=(4, 0))

    def _build_log_frame(self, parent: ttk.Frame) -> None:
        frm = ttk.LabelFrame(parent, text="串口接收 / 通信日志", padding=8)
        frm.grid(row=3, column=0, sticky="nsew")
        frm.columnconfigure(0, weight=1)
        frm.rowconfigure(0, weight=1)

        self.log_text = tk.Text(frm, height=14, wrap="none")
        self.log_text.grid(row=0, column=0, sticky="nsew")

        ybar = ttk.Scrollbar(frm, orient="vertical", command=self.log_text.yview)
        ybar.grid(row=0, column=1, sticky="ns")
        self.log_text.configure(yscrollcommand=ybar.set)

    def _build_status_bar(self, parent: ttk.Frame) -> None:
        bar = ttk.Frame(parent)
        # 放在 root 的最底部第 2 行，避免和主内容区(row=1)重叠遮挡。
        bar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        ttk.Label(bar, text="状态：").pack(side=tk.LEFT)
        ttk.Label(bar, textvariable=self.status_var).pack(side=tk.LEFT)

    def clear_history_records(self) -> None:
        if hasattr(self, "history_text"):
            self.history_text.delete("1.0", tk.END)

    def _append_history_record(self, text: str) -> None:
        if not hasattr(self, "history_text"):
            return
        self.history_text.insert(tk.END, text if text.endswith("\n") else text + "\n")
        self.history_text.see(tk.END)

    def query_history_records(self) -> None:
        """
        发送 LIST 查询历史记录。下位机返回 ASCII 结构化记录，
        上位机在“历史刷卡记录”区域转译为验收要求的中文说明。
        """
        if not self._serial_is_open():
            messagebox.showwarning("串口未连接", "请先打开串口，再查询历史记录")
            return

        self.clear_history_records()
        self._append_history_record("正在查询历史刷卡记录...")
        try:
            self._serial_write_line("LIST")
        except Exception as exc:
            self.log(f"[ERR] LIST 查询失败：{exc}")
            messagebox.showerror("LIST 查询失败", str(exc))

    def choose_avatar(self) -> None:
        path = filedialog.askopenfilename(
            title="选择头像图片",
            filetypes=[
                ("Image files", "*.jpg *.jpeg *.png *.bmp *.webp"),
                ("All files", "*.*"),
            ],
        )
        if path:
            self.avatar_var.set(path)

    def choose_font(self) -> None:
        path = filedialog.askopenfilename(
            title="选择字体文件",
            filetypes=[
                ("Font files", "*.ttf *.ttc *.otf"),
                ("All files", "*.*"),
            ],
        )
        if path:
            self.font_var.set(path)

    def log(self, text: str) -> None:
        self.log_queue.put(text if text.endswith("\n") else text + "\n")

    def clear_log(self) -> None:
        self.log_text.delete("1.0", tk.END)

    def refresh_serial_ports(self) -> None:
        """扫描当前主机串口，并尽量自动选中开发板 USB 串口。"""
        try:
            from serial.tools import list_ports  # type: ignore
        except Exception as exc:
            self.port_display_map = {}
            if hasattr(self, "port_combo"):
                self.port_combo.configure(values=())
            self.serial_status_var.set("未安装 pyserial，无法扫描串口")
            self.log(f"[ERR] 未安装 pyserial 或导入失败：{exc}")
            return

        preferred_keywords = (
            "CH340",
            "USB-SERIAL",
            "USB Serial",
            "STMicroelectronics",
            "STLink",
            "Virtual COM",
            "串口",
        )

        ports = list(list_ports.comports())
        displays: list[str] = []
        self.port_display_map = {}

        for info in ports:
            device = str(info.device)
            desc = (info.description or "").strip()
            hwid = (info.hwid or "").strip()
            display = f"{device} - {desc}" if desc else device
            self.port_display_map[display] = device
            displays.append(display)

        if hasattr(self, "port_combo"):
            self.port_combo.configure(values=displays)

        if not displays:
            self.port_var.set("")
            self.serial_status_var.set("未发现串口")
            self.log("[INFO] 未发现可用串口")
            return

        selected = displays[0]
        for display in displays:
            haystack = display.upper()
            if any(keyword.upper() in haystack for keyword in preferred_keywords):
                selected = display
                break

        self.port_var.set(selected)
        self.serial_status_var.set(f"已发现 {len(displays)} 个串口")
        self.log("[INFO] 串口列表已刷新：" + ", ".join(displays))

    def _selected_port_name(self) -> str:
        """从下拉框显示文本中取出真正的串口名，例如 'COM4 - CH340' -> 'COM4'。"""
        raw = self.port_var.get().strip()
        if not raw:
            return ""
        if raw in self.port_display_map:
            return self.port_display_map[raw]
        return raw.split(" - ", 1)[0].strip()

    def _serial_is_open(self) -> bool:
        return bool(self.serial_conn is not None and getattr(self.serial_conn, "is_open", False))

    def open_serial_port(self) -> None:
        """打开手动串口通信连接，用于 PING/验收演示和接收区实时显示。"""
        if self._serial_is_open():
            messagebox.showinfo("串口已打开", "当前串口已经打开")
            return

        port = self._selected_port_name()
        if not port:
            messagebox.showerror("串口错误", "请选择串口")
            return

        try:
            baud = int(self.baud_var.get())
        except Exception:
            messagebox.showerror("串口错误", "波特率非法")
            return

        try:
            serial = issue_card.import_pyserial()
            self.serial_conn = serial.Serial(port, baudrate=baud, timeout=0.1, write_timeout=1.0)
            time.sleep(0.2)
            with contextlib.suppress(Exception):
                self.serial_conn.reset_input_buffer()
            with contextlib.suppress(Exception):
                self.serial_conn.reset_output_buffer()

            self.rx_stop_event.clear()
            self.rx_thread = threading.Thread(target=self._serial_rx_loop, daemon=True)
            self.rx_thread.start()

            self.open_serial_btn.configure(state=tk.DISABLED)
            self.close_serial_btn.configure(state=tk.NORMAL)
            self.serial_status_var.set(f"已连接 {port} @ {baud}")
            self.log(f"[INFO] 已打开串口 {port} @ {baud}")
        except Exception as exc:
            self.serial_conn = None
            self.serial_status_var.set("串口打开失败")
            self.log(f"[ERR] 打开串口失败：{exc}")
            messagebox.showerror("打开串口失败", str(exc))

    def close_serial_port(self) -> None:
        """关闭手动串口通信连接。"""
        self.rx_stop_event.set()
        ser = self.serial_conn
        self.serial_conn = None
        if ser is not None:
            with contextlib.suppress(Exception):
                ser.close()

        if hasattr(self, "open_serial_btn"):
            self.open_serial_btn.configure(state=tk.NORMAL)
        if hasattr(self, "close_serial_btn"):
            self.close_serial_btn.configure(state=tk.DISABLED)
        self.serial_status_var.set("串口未连接")
        self.log("[INFO] 串口已关闭")

    def _serial_rx_loop(self) -> None:
        """后台接收线程：只向队列投递文本，不直接操作 Tk 控件。"""
        while not self.rx_stop_event.is_set():
            ser = self.serial_conn
            if ser is None:
                break

            try:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    self.log_queue.put(f"<<< {line}\n")

                    translated = translate_list_record_line(line)
                    if translated:
                        self.history_queue.put(translated + "\n")
                        self.log_queue.put(f"[LIST] {translated}\n")
                    elif is_list_boundary_line(line):
                        self.history_queue.put(line + "\n")
            except Exception as exc:
                if not self.rx_stop_event.is_set():
                    self.log_queue.put(f"[ERR] 串口接收异常：{exc}\n")
                    self.after(0, self.close_serial_port)
                break

    def _serial_write_line(self, line: str) -> None:
        if not self._serial_is_open():
            raise RuntimeError("请先打开串口")

        cmd = line.strip()
        if not cmd:
            return

        ser = self.serial_conn
        if ser is None:
            raise RuntimeError("串口未连接")

        ser.write((cmd + "\n").encode("ascii", errors="replace"))
        ser.flush()
        self.log(f">>> {cmd}")

    def send_manual_command(self) -> None:
        """手动发送一行命令，便于验收演示 PING/PONG。"""
        try:
            self._serial_write_line(self.manual_cmd_var.get())
        except Exception as exc:
            self.log(f"[ERR] 发送失败：{exc}")
            messagebox.showerror("发送失败", str(exc))

    def _close_live_serial_before_card_operation(self) -> None:
        """卡操作会独立打开串口；若手动串口连接打开，先关闭避免端口占用。"""
        if self._serial_is_open():
            self.log("[INFO] 发卡/读卡/清卡将独立打开串口，已先关闭手动串口连接")
            self.close_serial_port()

    def _poll_log_queue(self) -> None:
        try:
            while True:
                msg = self.log_queue.get_nowait()
                self.log_text.insert(tk.END, msg)
                self.log_text.see(tk.END)
        except queue.Empty:
            pass

        try:
            while True:
                msg = self.history_queue.get_nowait()
                self._append_history_record(msg)
        except queue.Empty:
            pass

        self.after(50, self._poll_log_queue)

    def set_busy(self, busy: bool, status: str | None = None) -> None:
        state = tk.DISABLED if busy else tk.NORMAL
        for btn in (
            self.preview_btn,
            self.dump_bin_btn,
            self.dump_blocks_btn,
            self.send_btn,
            self.read_btn,
            self.clear_card_btn,
            self.list_btn,
        ):
            btn.configure(state=state)

        save_btn = getattr(self, "save_read_payload_btn", None)
        if save_btn is not None:
            save_btn.configure(state=tk.DISABLED if busy or self.last_read_payload is None else tk.NORMAL)

        if status is not None:
            self.status_var.set(status)

    def set_operation_busy(self, busy: bool, status: str | None = None) -> None:
        """卡操作统一 busy 入口，复用原 set_busy。"""
        self.set_busy(busy, status)

    def build_payload_from_form(self) -> bytes:
        avatar = self.avatar_var.get().strip()
        name = self.name_var.get().strip()
        dept = self.dept_var.get().strip()
        font = self.font_var.get().strip()

        if not avatar:
            raise ValueError("请选择头像图片")
        if not name:
            raise ValueError("请输入姓名")
        if not dept:
            raise ValueError("请输入部门")

        font_path = issue_card.resolve_font_path(font if font else None)

        avatar_img = issue_card.render_avatar_bitmap(
            Path(avatar).expanduser(),
            size=issue_card.AVATAR_SIZE,
            threshold=int(self.avatar_threshold_var.get()),
            mode=self.avatar_mode_var.get(),
            dither=bool(self.dither_avatar_var.get()),
            invert=bool(self.invert_avatar_var.get()),
        )
        name_img = issue_card.render_text_bitmap(
            name,
            size=issue_card.TEXT_SIZE,
            font_path=font_path,
            threshold=int(self.text_threshold_var.get()),
            margin_x=int(self.text_margin_x_var.get()),
            invert=bool(self.invert_text_var.get()),
        )
        dept_img = issue_card.render_text_bitmap(
            dept,
            size=issue_card.TEXT_SIZE,
            font_path=font_path,
            threshold=int(self.text_threshold_var.get()),
            margin_x=int(self.text_margin_x_var.get()),
            invert=bool(self.invert_text_var.get()),
        )
        payload = issue_card.make_payload(
            avatar_img,
            name_img,
            dept_img,
            pack_mode=self.pack_var.get(),
            invert_all_bits=bool(self.invert_all_bits_var.get()),
        )
        return payload

    def make_preview_image(self, payload: bytes) -> Image.Image:
        return payload_to_preview_image(
            payload,
            pack_mode=self.pack_var.get(),
            scale=int(self.preview_scale_var.get()),
        )

    def update_preview(self, payload: bytes) -> None:
        img = self.make_preview_image(payload)
        self.last_preview_image = img
        self.preview_photo = ImageTk.PhotoImage(img)
        self.preview_label.configure(image=self.preview_photo, text="")

        crc = issue_card.crc16_xmodem(payload)
        self.summary_var.set(
            f"payload={len(payload)}B, blocks={issue_card.PAYLOAD_BLOCKS}, "
            f"CRC=0x{crc:04X}, pack={self.pack_var.get()}"
        )

    def on_preview(self) -> None:
        try:
            payload = self.build_payload_from_form()
            self.last_payload = payload
            self.update_preview(payload)
            self.log("OK: 预览已生成")
            self.status_var.set("预览完成")
        except Exception as exc:
            messagebox.showerror("生成预览失败", str(exc))
            self.log(f"ERROR: {exc}")
            self.status_var.set("预览失败")

    def get_or_build_payload(self) -> bytes:
        payload = self.build_payload_from_form()
        self.last_payload = payload
        self.update_preview(payload)
        return payload

    def on_dump_bin(self) -> None:
        try:
            payload = self.get_or_build_payload()
            path = filedialog.asksaveasfilename(
                title="保存 payload bin",
                defaultextension=".bin",
                filetypes=[("BIN files", "*.bin"), ("All files", "*.*")],
            )
            if not path:
                return
            out = Path(path).expanduser()
            issue_card.ensure_parent_dir(out)
            out.write_bytes(payload)
            self.log(f"OK: payload 已导出：{out}")
            self.status_var.set("导出 payload 完成")
        except Exception as exc:
            messagebox.showerror("导出失败", str(exc))
            self.log(f"ERROR: {exc}")
            self.status_var.set("导出失败")

    def on_dump_blocks(self) -> None:
        try:
            payload = self.get_or_build_payload()
            path = filedialog.asksaveasfilename(
                title="保存 blocks txt",
                defaultextension=".txt",
                filetypes=[("Text files", "*.txt"), ("All files", "*.*")],
            )
            if not path:
                return
            out = Path(path).expanduser()
            issue_card.dump_blocks_text(payload, out)
            self.log(f"OK: blocks 已导出：{out}")
            self.status_var.set("导出 blocks 完成")
        except Exception as exc:
            messagebox.showerror("导出失败", str(exc))
            self.log(f"ERROR: {exc}")
            self.status_var.set("导出失败")

    def on_save_read_payload(self) -> None:
        try:
            if self.last_read_payload is None:
                raise ValueError("当前没有读出的 payload，请先读取卡片")

            path = filedialog.asksaveasfilename(
                title="保存读出的 payload bin",
                defaultextension=".bin",
                filetypes=[("BIN files", "*.bin"), ("All files", "*.*")],
            )
            if not path:
                return

            out = Path(path).expanduser()
            issue_card.ensure_parent_dir(out)
            out.write_bytes(self.last_read_payload)
            self.log(f"OK: 读出 payload 已保存：{out}")
            self.status_var.set("保存读出 payload 完成")
        except Exception as exc:
            messagebox.showerror("保存失败", str(exc))
            self.log(f"ERROR: {exc}")
            self.status_var.set("保存失败")

    def on_read_card(self) -> None:
        if self.worker and self.worker.is_alive():
            messagebox.showwarning("正在操作", "当前已有发卡/读卡/清卡任务在运行")
            return

        try:
            port = self._selected_port_name()
            if not port:
                raise ValueError("请选择串口号")
            baud = int(self.baud_var.get())
            timeout = float(self.timeout_var.get())
            verbose = bool(self.verbose_var.get())
        except Exception as exc:
            messagebox.showerror("参数错误", str(exc))
            self.log(f"ERROR: {exc}")
            return

        self._close_live_serial_before_card_operation()
        self.set_operation_busy(True, "读卡中")
        self.log("--- 开始读取卡片 CARD_READ ---")

        self.worker = threading.Thread(
            target=self._read_worker,
            args=(port, baud, timeout, verbose),
            daemon=True,
        )
        self.worker.start()

    def _read_worker(self, port: str, baud: int, timeout: float, verbose: bool) -> None:
        writer = QueueWriter(self.log_queue)
        try:
            with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
                result = read_card_from_mcu(
                    port=port,
                    baud=baud,
                    timeout=timeout,
                    verbose=verbose,
                )
            self.log_queue.put("--- 读卡完成 ---\n")
            self.after(0, lambda r=result: self._on_read_success(r))
        except Exception as exc:
            self.log_queue.put("ERROR: 读卡失败\n")
            self.log_queue.put(str(exc) + "\n")
            self.log_queue.put(traceback.format_exc() + "\n")
            self.after(0, lambda e=exc: self._on_operation_error("读卡失败", "读卡失败", e))
        finally:
            writer.flush()

    def _on_read_success(self, result: CardReadResult) -> None:
        self.last_read_payload = result.payload
        self.last_payload = result.payload

        self.read_uid_var.set(result.uid)
        self.read_id_var.set(result.card_id)
        self.read_type_var.set(result.card_type)
        self.read_status_var.set(result.status)
        self.read_size_var.set(f"{result.size} B")
        self.read_crc_var.set(f"0x{result.crc:04X}")
        # 验收要求：读卡后自动把下位机上传的卡号/学号和卡类型填回编辑区。
        self.card_id_var.set(result.card_id)
        self.card_type_var.set(result.card_type)
        self.read_note_var.set("读卡成功：已自动填入学号/工号和卡类型，并按当前 pack 模式生成预览")

        self.update_preview(result.payload)
        self.set_operation_busy(False, "读卡完成")
        messagebox.showinfo(
            "读卡完成",
            f"UID: {result.uid}\n"
            f"ID: {result.card_id}\n"
            f"type: {result.card_type}\n"
            f"status: {result.status}\n"
            f"size: {result.size} B\n"
            f"CRC: 0x{result.crc:04X}",
        )

    def on_clear_card(self) -> None:
        if self.worker and self.worker.is_alive():
            messagebox.showwarning("正在操作", "当前已有发卡/读卡/清卡任务在运行")
            return

        ok = messagebox.askyesno(
            "确认销卡 / 清卡",
            "该操作会清空本系统写入的账户头、头像、姓名、部门数据，"
            "清空后该卡会被识别为无效卡。是否继续？",
        )
        if not ok:
            self.log("INFO: 用户取消清卡")
            return

        try:
            port = self._selected_port_name()
            if not port:
                raise ValueError("请选择串口号")
            baud = int(self.baud_var.get())
            timeout = float(self.timeout_var.get())
        except Exception as exc:
            messagebox.showerror("参数错误", str(exc))
            self.log(f"ERROR: {exc}")
            return

        self._close_live_serial_before_card_operation()
        self.set_operation_busy(True, "清卡中")
        self.log("--- 开始销卡 / 清卡 CARD_CLEAR ---")

        self.worker = threading.Thread(
            target=self._clear_worker,
            args=(port, baud, timeout),
            daemon=True,
        )
        self.worker.start()

    def _clear_worker(self, port: str, baud: int, timeout: float) -> None:
        writer = QueueWriter(self.log_queue)
        try:
            with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
                clear_card_on_mcu(port=port, baud=baud, timeout=timeout)
            self.log_queue.put("--- 清卡完成 ---\n")
            self.after(0, self._on_clear_success)
        except Exception as exc:
            self.log_queue.put("ERROR: 清卡失败\n")
            self.log_queue.put(str(exc) + "\n")
            self.log_queue.put(traceback.format_exc() + "\n")
            self.after(0, lambda e=exc: self._on_operation_error("清卡失败", "清卡失败", e))
        finally:
            writer.flush()

    def _on_clear_success(self) -> None:
        self.set_operation_busy(False, "清卡完成")
        messagebox.showinfo("清卡完成", "清卡成功：MCU 返回 OK CARD CLEAR DONE")

    def _on_operation_error(self, title: str, status: str, exc: Exception) -> None:
        self.set_operation_busy(False, status)
        messagebox.showerror(title, str(exc))


    def on_send(self) -> None:
        if self.worker and self.worker.is_alive():
            messagebox.showwarning("正在操作", "当前已有发卡/读卡/清卡任务在运行")
            return

        try:
            payload = self.get_or_build_payload()
            port = self._selected_port_name()
            card_id = self.card_id_var.get().strip()
            if not port:
                raise ValueError("请选择串口号")
            if not card_id:
                raise ValueError("请输入学号/工号")
        except Exception as exc:
            messagebox.showerror("参数错误", str(exc))
            self.log(f"ERROR: {exc}")
            return

        self._close_live_serial_before_card_operation()
        self.set_operation_busy(True, "发送中")
        self.log("--- 开始发送到 MCU ---")

        self.worker = threading.Thread(
            target=self._send_worker,
            args=(
                payload,
                port,
                int(self.baud_var.get()),
                float(self.timeout_var.get()),
                self.card_type_var.get(),
                card_id,
                bool(self.commit_var.get()),
                bool(self.verbose_var.get()),
            ),
            daemon=True,
        )
        self.worker.start()

    def _send_worker(
        self,
        payload: bytes,
        port: str,
        baud: int,
        timeout: float,
        card_type: str,
        card_id: str,
        commit: bool,
        verbose: bool,
    ) -> None:
        writer = QueueWriter(self.log_queue)
        try:
            with contextlib.redirect_stdout(writer), contextlib.redirect_stderr(writer):
                issue_card.send_issue_sequence(
                    payload=payload,
                    port=port,
                    baud=baud,
                    timeout=timeout,
                    card_type=card_type,
                    card_id=card_id,
                    commit=commit,
                    verbose=verbose,
                )
            self.log_queue.put("--- 发送完成 ---\n")
            self.after(0, lambda: self.set_operation_busy(False, "发送完成"))
            self.after(0, lambda: messagebox.showinfo("发送完成", "payload 已成功发送到 MCU"))
        except Exception as exc:
            self.log_queue.put("ERROR: 发送失败\n")
            self.log_queue.put(str(exc) + "\n")
            self.log_queue.put(traceback.format_exc() + "\n")
            self.after(0, lambda: self.set_operation_busy(False, "发送失败"))
            self.after(0, lambda: messagebox.showerror("发送失败", str(exc)))
        finally:
            writer.flush()

    def on_close(self) -> None:
        """窗口关闭时释放串口资源。"""
        self.close_serial_port()
        self.destroy()


def main() -> int:
    app = IssueCardGUI()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
