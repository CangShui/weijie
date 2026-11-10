"""深色背景白字窗口，用于复现结果面板背景透出造成的文字重影。"""
import tkinter as tk

root = tk.Tk()
root.title("OCR Dark Text")
root.geometry("1300x920+80+40")
root.configure(bg="black")
root.attributes("-topmost", True)
lines = [f"第{i:02d}行 提取文本会造成文本重叠 测试编号{i * 137:04d}"
         for i in range(1, 35)]
for i, line in enumerate(lines):
    tk.Label(root, text=line, bg="black", fg="white",
             font=("Microsoft YaHei UI", 13), anchor="w").place(
        x=30, y=12 + i * 25, width=1240, height=23)
root.mainloop()
