import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
import serial
import serial.tools.list_ports
import threading
import time
import binascii

class SerialAssistant:
    def __init__(self, root):
        self.root = root
        self.root.title("Python串口助手")
        self.root.geometry("800x600")
        
        # 串口对象
        self.serial = None
        # 是否停止接收线程
        self.is_receiving = False
        # 接收字节计数
        self.receive_count = 0
        
        # 添加复选框：自动添加\r\n
        self.add_crlf_var = tk.BooleanVar()  # 布尔变量跟踪复选框状态
        self.add_crlf_checkbox = tk.Checkbutton(root, text="自动添加换行 (\\r\\n)", variable=self.add_crlf_var)
        self.add_crlf_checkbox.pack()  # 或使用grid/place布局，根据您的GUI布局调整
        
        self.create_widgets()
        self.scan_serial_ports()
        
    def create_widgets(self):
        # 主框架划分
        frame_left = ttk.Frame(self.root, padding=10)
        frame_left.pack(side=tk.LEFT, fill=tk.BOTH, expand=False)
        
        frame_right = ttk.Frame(self.root, padding=10)
        frame_right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        # 左侧：串口设置区域
        settings_frame = ttk.LabelFrame(frame_left, text="串口设置", padding=10)
        settings_frame.pack(fill=tk.BOTH, expand=True)
        
        # 串口选择
        ttk.Label(settings_frame, text="串口:").grid(column=0, row=0, sticky=tk.W, pady=5)
        self.port_combo = ttk.Combobox(settings_frame, width=15)
        self.port_combo.grid(column=1, row=0, pady=5)
        
        # 刷新串口按钮
        self.refresh_btn = ttk.Button(settings_frame, text="刷新", command=self.scan_serial_ports)
        self.refresh_btn.grid(column=0, columnspan=2, row=1, pady=5, sticky=tk.EW)
        
        # 波特率选择
        ttk.Label(settings_frame, text="波特率:").grid(column=0, row=2, sticky=tk.W, pady=5)
        self.baudrate_combo = ttk.Combobox(settings_frame, width=15)
        self.baudrate_combo['values'] = ('9600', '19200', '38400', '57600', '115200')
        self.baudrate_combo.current(0)
        self.baudrate_combo.grid(column=1, row=2, pady=5)
        
        # 数据位选择
        ttk.Label(settings_frame, text="数据位:").grid(column=0, row=3, sticky=tk.W, pady=5)
        self.data_bits_combo = ttk.Combobox(settings_frame, width=15)
        self.data_bits_combo['values'] = ('5', '6', '7', '8')
        self.data_bits_combo.current(3)  # 默认8位
        self.data_bits_combo.grid(column=1, row=3, pady=5)
        
        # 校验位选择
        ttk.Label(settings_frame, text="校验位:").grid(column=0, row=4, sticky=tk.W, pady=5)
        self.parity_combo = ttk.Combobox(settings_frame, width=15)
        self.parity_combo['values'] = ('无', '奇校验', '偶校验')
        self.parity_combo.current(0)
        self.parity_combo.grid(column=1, row=4, pady=5)
        
        # 停止位选择
        ttk.Label(settings_frame, text="停止位:").grid(column=0, row=5, sticky=tk.W, pady=5)
        self.stop_bits_combo = ttk.Combobox(settings_frame, width=15)
        self.stop_bits_combo['values'] = ('1', '1.5', '2')
        self.stop_bits_combo.current(0)
        self.stop_bits_combo.grid(column=1, row=5, pady=5)
        
        # 打开串口按钮
        self.open_btn = ttk.Button(settings_frame, text="打开串口", command=self.toggle_serial)
        self.open_btn.grid(column=0, columnspan=2, row=6, pady=10, sticky=tk.EW)
        
        # 右侧：数据收发区域
        # 接收区
        receive_frame = ttk.LabelFrame(frame_right, text="接收区", padding=10)
        receive_frame.pack(fill=tk.BOTH, expand=True)
        
        receive_top_frame = ttk.Frame(receive_frame)
        receive_top_frame.pack(fill=tk.X)
        
        # 接收方式
        self.receive_mode = tk.StringVar(value="ASCII")
        ttk.Radiobutton(receive_top_frame, text="ASCII", variable=self.receive_mode, value="ASCII").pack(side=tk.LEFT)
        ttk.Radiobutton(receive_top_frame, text="HEX", variable=self.receive_mode, value="HEX").pack(side=tk.LEFT)
        
        # 清除接收区按钮
        ttk.Button(receive_top_frame, text="清除接收", command=self.clear_receive).pack(side=tk.RIGHT)
        
        # 接收计数显示
        self.receive_count_var = tk.StringVar(value="接收: 0 字节")
        ttk.Label(receive_top_frame, textvariable=self.receive_count_var).pack(side=tk.RIGHT, padx=10)
        
        # 接收文本区
        self.receive_text = scrolledtext.ScrolledText(receive_frame, height=15)
        self.receive_text.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # 发送区
        send_frame = ttk.LabelFrame(frame_right, text="发送区", padding=10)
        send_frame.pack(fill=tk.BOTH, expand=True)
        
        send_top_frame = ttk.Frame(send_frame)
        send_top_frame.pack(fill=tk.X)
        
        # 发送方式
        self.send_mode = tk.StringVar(value="ASCII")
        ttk.Radiobutton(send_top_frame, text="ASCII", variable=self.send_mode, value="ASCII").pack(side=tk.LEFT)
        ttk.Radiobutton(send_top_frame, text="HEX", variable=self.send_mode, value="HEX").pack(side=tk.LEFT)
        
        # 发送文本区
        self.send_text = scrolledtext.ScrolledText(send_frame, height=5)
        self.send_text.pack(fill=tk.BOTH, expand=True, pady=5)
        
        # 发送按钮
        send_button_frame = ttk.Frame(send_frame)
        send_button_frame.pack(fill=tk.X)
        
        self.send_btn = ttk.Button(send_button_frame, text="发送", command=self.send_data, state=tk.DISABLED)
        self.send_btn.pack(side=tk.RIGHT, padx=5)
        
    def scan_serial_ports(self):
        """扫描可用串口"""
        ports = [port.device for port in serial.tools.list_ports.comports()]
        self.port_combo['values'] = ports
        if ports:
            self.port_combo.current(0)
    
    def toggle_serial(self):
        """打开/关闭串口"""
        if self.serial is None or not self.serial.is_open:
            self.open_serial()
        else:
            self.close_serial()
    
    def open_serial(self):
        """打开串口"""
        port = self.port_combo.get()
        if not port:
            messagebox.showerror("错误", "请选择串口")
            return
        
        baudrate = int(self.baudrate_combo.get())
        data_bits = int(self.data_bits_combo.get())
        
        parity_map = {'无': serial.PARITY_NONE, '奇校验': serial.PARITY_ODD, '偶校验': serial.PARITY_EVEN}
        parity = parity_map.get(self.parity_combo.get(), serial.PARITY_NONE)
        
        stop_bits_map = {'1': serial.STOPBITS_ONE, '1.5': serial.STOPBITS_ONE_POINT_FIVE, '2': serial.STOPBITS_TWO}
        stop_bits = stop_bits_map.get(self.stop_bits_combo.get(), serial.STOPBITS_ONE)
        
        try:
            self.serial = serial.Serial(
                port=port,
                baudrate=baudrate,
                bytesize=data_bits,
                parity=parity,
                stopbits=stop_bits,
                timeout=0.1
            )
            
            if self.serial.is_open:
                self.open_btn.config(text="关闭串口")
                self.send_btn.config(state=tk.NORMAL)
                self.is_receiving = True
                
                # 启动接收线程
                self.receive_thread = threading.Thread(target=self.receive_data)
                self.receive_thread.daemon = True
                self.receive_thread.start()
                
                messagebox.showinfo("成功", f"成功打开串口 {port}")
        except Exception as e:
            messagebox.showerror("错误", f"无法打开串口: {str(e)}")
    
    def close_serial(self):
        """关闭串口"""
        if self.serial and self.serial.is_open:
            self.is_receiving = False
            time.sleep(0.2)  # 给接收线程一点时间关闭
            self.serial.close()
            self.serial = None
            self.open_btn.config(text="打开串口")
            self.send_btn.config(state=tk.DISABLED)
            messagebox.showinfo("成功", "串口已关闭")
    
    def receive_data(self):
        """接收数据线程"""
        while self.is_receiving and self.serial and self.serial.is_open:
            try:
                # 读取所有可用数据
                data = self.serial.read_all()
                if data:
                    # 更新接收计数
                    self.receive_count += len(data)
                    self.receive_count_var.set(f"接收: {self.receive_count} 字节")
                    
                    # 根据接收模式显示数据
                    if self.receive_mode.get() == "HEX":
                        hex_data = ' '.join([f"{b:02X}" for b in data])
                        self.update_receive_text(hex_data + ' ')
                    else:  # ASCII模式
                        try:
                            text_data = data.decode('utf-8', errors='replace')
                            self.update_receive_text(text_data)
                        except UnicodeDecodeError:
                            text_data = data.decode('latin-1')
                            self.update_receive_text(text_data)
            except Exception as e:
                print(f"接收数据错误: {str(e)}")
                break
            time.sleep(0.01)
    
    def update_receive_text(self, text):
        """更新接收区文本（线程安全）"""
        self.root.after(0, lambda: self._update_text(text))
    
    def _update_text(self, text):
        """实际更新文本区的函数，自动滚动清除旧数据"""
        self.receive_text.configure(state=tk.NORMAL)
        self.receive_text.insert(tk.END, text)
        self.receive_text.see(tk.END)
        
        # 滚动清除：只保留最近 max_chars 个字符
        max_chars = 50000  # 可根据需要调整
        current_length = int(self.receive_text.index('end-1c').split('.')[0])
        all_text = self.receive_text.get("1.0", tk.END)
        if len(all_text) > max_chars:
            # 删除最前面的多余字符
            self.receive_text.delete("1.0", f"1.{len(all_text) - max_chars}")
        
        self.receive_text.configure(state=tk.NORMAL)
    
    def send_data(self):
        """发送数据"""
        if not self.serial or not self.serial.is_open:
            messagebox.showerror("错误", "串口未打开")
            return
        
        send_text = self.send_text.get("1.0", tk.END).strip()
        if not send_text:
            return
        
        try:
            if self.send_mode.get() == "HEX":
                # 移除可能的空格并解析十六进制字符串
                hex_str = send_text.replace(" ", "")
                if len(hex_str) % 2 != 0:
                    messagebox.showerror("错误", "HEX格式错误: 长度必须是2的倍数")
                    return
                
                try:
                    data = binascii.unhexlify(hex_str)
                except binascii.Error:
                    messagebox.showerror("错误", "无效的HEX字符串")
                    return
            else:  # ASCII模式
                data = send_text.encode('utf-8')
            
            # 检查复选框状态，决定是否添加\r\n
            if self.add_crlf_var.get():
                data += b'\r\n'
            
            self.serial.write(data)
        except Exception as e:
            messagebox.showerror("错误", f"发送数据错误: {str(e)}")
    
    def clear_receive(self):
        """清除接收区"""
        self.receive_text.configure(state=tk.NORMAL)
        self.receive_text.delete("1.0", tk.END)
        self.receive_text.configure(state=tk.NORMAL)
        self.receive_count = 0
        self.receive_count_var.set(f"接收: {self.receive_count} 字节")

if __name__ == "__main__":
    # 检查是否安装了必要的库
    try:
        import serial
        import tkinter
    except ImportError:
        print("请安装必要的库:")
        print("pip install pyserial")
        exit(1)
        
    root = tk.Tk()
    app = SerialAssistant(root)
    root.protocol("WM_DELETE_WINDOW", lambda: (app.close_serial(), root.destroy()))
    root.mainloop()