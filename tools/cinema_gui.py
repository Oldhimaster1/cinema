#!/usr/bin/env python3
"""Desktop GUI for the Cinema converter. Requires Python 3 tkinter, Pillow, FFmpeg.

The GUI invokes the same tested CLI encoder and verifier as automation. It
never edits calculator storage or writes directly to a USB device.
"""
from __future__ import annotations

import queue
import os
import signal
import subprocess
import sys
import threading
from pathlib import Path
from typing import Callable, Optional


def build_encode_command(video: str, output: str, *, fps: str = "15",
                         fit: str = "contain", dither: str = "clean",
                         palette: str = "median", packed4: bool = False,
                         subtitles: str = "", jobs: int = 0) -> list[str]:
    """Pure command builder, safe for filenames with spaces/metacharacters."""
    from encode_cin2 import parse_fps

    if not video.strip() or not output.strip():
        raise ValueError("Select an input video and an output .bin file")
    try:
        num, den = parse_fps(fps)
    except (ValueError, ZeroDivisionError) as exc:
        raise ValueError("FPS must be a positive number or fraction N/D") from exc
    if num <= 0 or den <= 0 or num / den > 60:
        raise ValueError("FPS must be a positive number no greater than 60")
    if fit not in ("stretch", "contain", "cover"):
        raise ValueError("Unknown aspect-ratio mode")
    if dither not in ("clean", "stable", "legacy"):
        raise ValueError("Unknown dither mode")
    if palette not in ("median", "weighted"):
        raise ValueError("Unknown palette method")
    if jobs < 0 or jobs > 64:
        raise ValueError("Worker count must be 0 to 64")
    cmd = [sys.executable, str(Path(__file__).with_name("encode_cin2.py")),
           video, output, "--fps", fps, "--fit", fit,
           "--dither", dither, "--palette-method", palette]
    if packed4:
        cmd.append("--packed4")
    if subtitles.strip():
        if Path(video).is_dir():
            raise ValueError("Subtitle conversion requires a single input video")
        cmd.extend(("--subtitles", subtitles))
    if jobs:
        cmd.extend(("--jobs", str(jobs)))
    return cmd


class CinemaConverterApp:
    def __init__(self, root):
        import tkinter as tk
        from tkinter import ttk

        self.root = root
        self.root.title("Cinema — Desktop Converter")
        self.root.minsize(625, 480)
        self.video = tk.StringVar()
        self.output = tk.StringVar()
        self.subtitles = tk.StringVar()
        self.fps = tk.StringVar(value="15")
        self.fit = tk.StringVar(value="contain")
        self.dither = tk.StringVar(value="clean")
        self.palette = tk.StringVar(value="median")
        self.packed4 = tk.BooleanVar(value=False)
        self.jobs = tk.StringVar(value="0")
        self.events: queue.Queue[tuple[str, str]] = queue.Queue()
        self.process: Optional[subprocess.Popen] = None
        self.running = False
        self.cancel_requested = False
        self.close_when_done = False

        frm = ttk.Frame(root, padding=14)
        frm.pack(fill="both", expand=True)
        frm.columnconfigure(1, weight=1)

        def path_row(row: int, title: str, variable, browse: Callable):
            ttk.Label(frm, text=title).grid(row=row, column=0, sticky="w", pady=5)
            ttk.Entry(frm, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=8)
            ttk.Button(frm, text="Browse…", command=browse).grid(row=row, column=2)

        path_row(0, "Video", self.video, self._browse_video)
        path_row(1, "Output .bin", self.output, self._browse_output)
        path_row(2, "Subtitles .srt (optional)", self.subtitles, self._browse_subtitles)

        options = ttk.Frame(frm)
        options.grid(row=3, column=0, columnspan=3, sticky="ew", pady=12)
        for col in range(4):
            options.columnconfigure(col, weight=1)

        for col, (label, variable, choices) in enumerate((
            ("Aspect ratio", self.fit, ("contain", "cover", "stretch")),
            ("Dithering", self.dither, ("clean", "stable", "legacy")),
            ("Palette", self.palette, ("median", "weighted")),
        )):
            ttk.Label(options, text=label).grid(row=0, column=col, sticky="w")
            ttk.Combobox(options, textvariable=variable, values=choices,
                         state="readonly", width=15).grid(row=1, column=col, sticky="ew", padx=(0, 8))
        ttk.Label(options, text="FPS").grid(row=0, column=3, sticky="w")
        ttk.Entry(options, textvariable=self.fps, width=12).grid(row=1, column=3, sticky="ew")

        row = ttk.Frame(frm)
        row.grid(row=4, column=0, columnspan=3, sticky="w")
        ttk.Checkbutton(row, text="Packed4: half-size frames (requires fixed_asm calculator build)",
                        variable=self.packed4).pack(side="left")
        ttk.Label(row, text="Workers (0=auto):").pack(side="left", padx=(12, 4))
        ttk.Entry(row, textvariable=self.jobs, width=4).pack(side="left")

        actions = ttk.Frame(frm)
        actions.grid(row=5, column=0, columnspan=3, sticky="ew", pady=8)
        self.convert_button = ttk.Button(actions, text="Convert video", command=self._start)
        self.convert_button.pack(side="left")
        self.cancel_button = ttk.Button(actions, text="Cancel", command=self._cancel,
                                        state="disabled")
        self.cancel_button.pack(side="left", padx=8)
        ttk.Label(actions, text="Creates an ordinary .bin file; does not write to USB.").pack(side="left")

        self.status = tk.StringVar(value="Ready")
        ttk.Label(frm, textvariable=self.status).grid(row=6, column=0, columnspan=3, sticky="w")
        self.log = tk.Text(frm, height=12, wrap="word", state="disabled")
        self.log.grid(row=7, column=0, columnspan=3, sticky="nsew", pady=(4, 0))
        frm.rowconfigure(7, weight=1)
        self.root.after(120, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._close)

    def _browse_video(self):
        from tkinter import filedialog
        chosen = filedialog.askopenfilename(title="Choose video")
        if chosen:
            self.video.set(chosen)
            if not self.output.get():
                self.output.set(str(Path(chosen).with_suffix(".bin")))

    def _browse_output(self):
        from tkinter import filedialog
        chosen = filedialog.asksaveasfilename(defaultextension=".bin",
                                              filetypes=[("Cinema movie", "*.bin"), ("All files", "*.*")])
        if chosen:
            self.output.set(chosen)

    def _browse_subtitles(self):
        from tkinter import filedialog
        chosen = filedialog.askopenfilename(filetypes=[("SRT subtitles", "*.srt")])
        if chosen:
            self.subtitles.set(chosen)

    def _append(self, message: str):
        self.log.configure(state="normal")
        self.log.insert("end", message + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _start(self):
        from tkinter import messagebox
        try:
            command = build_encode_command(
                self.video.get(), self.output.get(), fps=self.fps.get(),
                fit=self.fit.get(), dither=self.dither.get(), palette=self.palette.get(),
                packed4=self.packed4.get(), subtitles=self.subtitles.get(),
                jobs=int(self.jobs.get()))
        except (ValueError, OSError) as exc:
            messagebox.showerror("Invalid settings", str(exc))
            return
        self.running = True
        self.cancel_requested = False
        self.status.set("Converting…")
        self.convert_button.configure(state="disabled")
        self.cancel_button.configure(state="normal")
        self._append("Starting: " + str(Path(self.video.get()).name))
        threading.Thread(target=self._worker, args=(command,), daemon=True).start()

    def _worker(self, command: list[str]):
        try:
            # stdout is line-buffered on the reader side; avoids GUI hangs.
            creationflags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, text=True,
                                            bufsize=1, creationflags=creationflags)
            if self.cancel_requested:
                self._signal_cancel(self.process)
            assert self.process.stdout is not None
            for line in self.process.stdout:
                self.events.put(("log", line.rstrip()))
            status = self.process.wait()
            message = ("Cancelled; encoder will clean up its temporary file"
                       if self.cancel_requested else
                       "Completed successfully" if status == 0 else
                       f"Conversion exited with code {status}")
            self.events.put(("done", message))
        except OSError as exc:
            self.events.put(("done", str(exc)))
        finally:
            self.process = None

    def _cancel(self):
        self.cancel_requested = True
        process = self.process
        if process and process.poll() is None:
            self._signal_cancel(process)
        self.status.set("Stopping conversion safely…")

    @staticmethod
    def _signal_cancel(process: subprocess.Popen):
        # SIGINT raises KeyboardInterrupt in the encoder, which removes its
        # .partial output in the same exception handler used for disk errors.
        # SIGTERM on Unix / TerminateProcess on Windows bypasses that cleanup.
        try:
            process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt"
                                else signal.SIGINT)
        except (ProcessLookupError, OSError):
            pass

    def _poll(self):
        try:
            while True:
                kind, message = self.events.get_nowait()
                self._append(message)
                if kind == "done":
                    self.running = False
                    self.status.set(message)
                    self.convert_button.configure(state="normal")
                    self.cancel_button.configure(state="disabled")
                    if self.close_when_done:
                        self.root.destroy()
                        return
        except queue.Empty:
            pass
        self.root.after(120, self._poll)

    def _close(self):
        if self.running:
            from tkinter import messagebox
            if not messagebox.askyesno("Conversion running", "Cancel conversion and close?"):
                return
            self.close_when_done = True
            self._cancel()
        else:
            self.root.destroy()


def main() -> None:
    import tkinter as tk
    root = tk.Tk()
    CinemaConverterApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
