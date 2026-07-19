from __future__ import annotations

import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import messagebox, ttk


# This script is intended to be launched from the repository root.
# The executable and default config paths are kept here so the UI only needs
# dir, project, name, and the experimental postprocess condition.
REPO_ROOT = Path(__file__).resolve().parent
APP_EXE = REPO_ROOT / "out" / "build" / "default" / "Release" / "DualIC4VarjoApp.exe"

LEFT_JSON = Path(r"C:\Users\MiyafujiLab2\Downloads\gamma1.json")
RIGHT_JSON = Path(r"C:\Users\MiyafujiLab2\Downloads\gamma1.json")
CALIB_JSON = Path(r"C:\Users\MiyafujiLab2\Downloads\stereo_calibration.json")

CONDITION_TO_POSTPROCESS = {
    "none": "none",
    "blur": "blur",
    # The application-side mode name is "darken". The experiment condition name
    # shown to the user is "highlight".
    "highlight": "darken",
}

BUTTONS = [
    ("warmup", "_warmup", "none"),
    ("pre-test", "_pre-test", "none"),
    ("train1", "_train1", "selected"),
    ("train2", "_train2", "selected"),
    ("train3", "_train3", "selected"),
    ("post-test", "_post-test", "none"),
    ("practice", "_practice", "none"),
]


def build_command(output_dir: str, project: str, name: str, postprocess: str) -> list[str]:
    return [
        str(APP_EXE),
        "--dir", output_dir,
        "--project", project,
        "--name", name,
        "--left-device-index", "1",
        "--right-device-index", "0",
        "--left-json", str(LEFT_JSON),
        "--right-json", str(RIGHT_JSON),
        "--left-json-device-index", "0",
        "--right-json-device-index", "0",
        "--left-offset-x", "236",
        "--left-offset-y", "0",
        "--right-offset-x", "236",
        "--right-offset-y", "0",
        "--fps", "160",
        "--camera-start-delay-ms", "0",
        "--sync-timestamp", "host",
        "--sync-tolerance-ms", "5.0",
        "--calib", str(CALIB_JSON),
        "--calib-board-cols", "9",
        "--calib-board-rows", "12",
        "--calib-min-samples", "12",
        "--calib-capture-interval-ms", "500",
        "--calib-min-motion-px", "20",
        "--postprocess", postprocess,
        "--pc-preview", "1",
        "--pc-preview-width", "1600",
        "--pc-preview-height", "800",
        "--pc-preview-vsync", "1",
        "--d3d12-debug", "0",
    ]


def command_for_display(command: list[str]) -> str:
    return subprocess.list2cmdline(command)


class LauncherApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Dual IC4 Varjo Experiment Launcher")
        self.resizable(False, False)

        self.dir_var = tk.StringVar(value="logs")
        self.project_var = tk.StringVar(value="checkerboard_calibration_test")
        self.name_var = tk.StringVar(value="DEFAULT")
        self.condition_var = tk.StringVar(value="none")
        self.last_process: subprocess.Popen[bytes] | None = None

        self._build_ui()

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.grid(row=0, column=0, sticky="nsew")

        entries = [
            ("dir", self.dir_var),
            ("project", self.project_var),
            ("name", self.name_var),
        ]
        for row, (label, variable) in enumerate(entries):
            ttk.Label(root, text=label).grid(row=row, column=0, sticky="w", padx=(0, 8), pady=4)
            ttk.Entry(root, textvariable=variable, width=54).grid(row=row, column=1, columnspan=3, sticky="ew", pady=4)

        ttk.Label(root, text="condition").grid(row=3, column=0, sticky="w", padx=(0, 8), pady=4)
        condition = ttk.Combobox(
            root,
            textvariable=self.condition_var,
            values=("none", "blur", "highlight"),
            state="readonly",
            width=20,
        )
        condition.grid(row=3, column=1, sticky="w", pady=4)

        button_frame = ttk.LabelFrame(root, text="Launch", padding=8)
        button_frame.grid(row=4, column=0, columnspan=4, sticky="ew", pady=(12, 4))
        for index, (label, suffix, source) in enumerate(BUTTONS):
            button = ttk.Button(
                button_frame,
                text=label,
                width=14,
                command=lambda s=suffix, src=source, lbl=label: self.launch(lbl, s, src),
            )
            button.grid(row=index // 4, column=index % 4, padx=4, pady=4)

        status_frame = ttk.LabelFrame(root, text="Status", padding=8)
        status_frame.grid(row=5, column=0, columnspan=4, sticky="ew", pady=(8, 0))
        self.status = tk.Text(status_frame, width=88, height=8, wrap="word")
        self.status.grid(row=0, column=0, sticky="ew")
        self.status.insert("end", f"Repository root: {REPO_ROOT}\n")
        self.status.insert("end", "Waiting for launch.\n")
        self.status.configure(state="disabled")

    def append_status(self, text: str) -> None:
        self.status.configure(state="normal")
        self.status.insert("end", text)
        self.status.see("end")
        self.status.configure(state="disabled")

    def resolve_values(self, suffix: str, source: str) -> tuple[str, str, str, str, str]:
        output_dir = self.dir_var.get().strip()
        base_project = self.project_var.get().strip()
        name = self.name_var.get().strip()
        condition = self.condition_var.get().strip()

        if not output_dir:
            raise ValueError("dir is empty")
        if not base_project:
            raise ValueError("project is empty")
        if not name:
            raise ValueError("name is empty")
        if condition not in CONDITION_TO_POSTPROCESS:
            raise ValueError(f"unknown condition: {condition}")

        selected_condition = condition if source == "selected" else "none"
        postprocess = CONDITION_TO_POSTPROCESS[selected_condition]
        project = f"{base_project}{suffix}"
        return output_dir, project, name, selected_condition, postprocess

    def launch(self, label: str, suffix: str, source: str) -> None:
        try:
            output_dir, project, name, selected_condition, postprocess = self.resolve_values(suffix, source)
        except ValueError as error:
            messagebox.showerror("Input error", str(error))
            return

        if not APP_EXE.is_file():
            messagebox.showerror(
                "Executable not found",
                f"DualIC4VarjoApp.exe was not found:\n{APP_EXE}\n\nBuild Release first.",
            )
            return
        if not LEFT_JSON.is_file():
            messagebox.showerror("Config not found", f"Left JSON was not found:\n{LEFT_JSON}")
            return
        if not RIGHT_JSON.is_file():
            messagebox.showerror("Config not found", f"Right JSON was not found:\n{RIGHT_JSON}")
            return
        if not CALIB_JSON.is_file():
            messagebox.showerror("Calibration not found", f"Calibration JSON was not found:\n{CALIB_JSON}")
            return

        command = build_command(output_dir, project, name, postprocess)
        creationflags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
        try:
            self.last_process = subprocess.Popen(
                command,
                cwd=str(REPO_ROOT),
                creationflags=creationflags,
            )
        except OSError as error:
            messagebox.showerror("Launch failed", str(error))
            return

        self.append_status(
            "\n"
            f"[{label}] launched\n"
            f"project: {project}\n"
            f"name: {name}\n"
            f"condition: {selected_condition}\n"
            f"--postprocess: {postprocess}\n"
            f"pid: {self.last_process.pid}\n"
            f"command: {command_for_display(command)}\n"
        )


def main() -> int:
    app = LauncherApp()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
