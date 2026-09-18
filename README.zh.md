# hentrOS

[English](README.md) | [Русский](README.ru.md) | **中文**

一个带有类 Windows 图形桌面的迷你业余玩具操作系统，通过自制的 UEFI 引导程序
启动。完全从零用 freestanding C 编写——没有借用任何操作系统代码，也不依赖
外部 EFI SDK。

这是一个教学用的玩具操作系统演示，不是生产环境或通用操作系统。它没有文件
系统驱动、没有进程模型，也没有网络功能。

## 项目内容

hentrOS 是一个单一的 UEFI 应用程序（`BOOTX64.EFI`）——它从不调用
`ExitBootServices`，所以没有独立的内核阶段。它通过一小部分手写的 UEFI API
子集（`boot/efi.h`，不依赖 gnu-efi/edk2）直接与固件通信：

- `boot/boot.c` —— 选择 Graphics Output Protocol 视频模式，绘制启动菜单，
  然后运行整个桌面：渐变壁纸、带开始按钮和实时时钟的任务栏
  （`EFI_RUNTIME_SERVICES.GetTime`）、可拖动且带关闭按钮的窗口，以及开始
  菜单。
- `kernel/gfx.c`、`kernel/font.c` —— 一个小型软件光栅化器（矩形、线条、
  渐变、自制的 5x7 位图字体），绘制到离屏缓冲区，再由 `gfx_present()` 在
  每一帧把它一次性搬运到真实的帧缓冲区。离屏绘制再原子式呈现，正是让屏幕
  不闪烁、不撕裂的关键——直接在显示器同时在扫描输出的帧缓冲区上绘制图形
  会造成明显的画面撕裂，机器画得越慢，撕裂越明显。
- `kernel/logo.c` —— 对项目手绘黄色马克笔草图 logo（带有八片旋转花瓣的
  网状中心）的风格化、程序化再现，基于纯整数的螺旋数学计算（不使用 libm）。

**输入**通过 UEFI 自身的协议实现，而不是直接访问硬件端口：鼠标使用
`EFI_SIMPLE_POINTER_PROTOCOL`（回退到 `EFI_ABSOLUTE_POINTER_PROTOCOL`），
键盘使用 `EFI_SIMPLE_TEXT_INPUT_PROTOCOL`。这很关键，因为 UEFI 自身的驱动
栈理解 USB HID 设备——如今几乎所有真实硬件都是如此——而手写的 PS/2 端口
驱动一旦启动固件移交控制权，根本看不到 USB 鼠标或键盘。方向键 + Enter/
空格键始终作为整个桌面的纯键盘回退方案，以防找不到任何指针设备。

不同主板暴露指针设备的方式差异很大——有些开机几秒后才完成 USB 枚举，
有些会发布不止一个指针实例（触摸板和 USB 鼠标同时存在），还有一些只会
暴露绝对坐标（absolute）版本。`boot/boot.c` 中的 `desktop_loop()` 通过
以下方式处理这些情况：强制整个驱动树连接（`connect_all_controllers()`，
和 UEFI Shell 的 `connect -r` 做的事情一样，因为有些主板只会按需绑定
USB HID 驱动）、枚举两种指针协议的*每一个*句柄，而不是直接使用
`LocateProtocol` 随手返回的第一个、在启动时把整个扫描过程重试几秒钟，
并在运行期间定期重新扫描，以应对热插拔或延迟完成枚举的设备。

如果这样处理之后鼠标仍然不动，很可能是 BIOS/UEFI 的设置问题，而不是操作
系统能解决的：检查并**禁用 "Fast Boot"**，确保 **"Legacy USB Support"** /
**"USB Configuration"** 设置为完全启用而不是 "boot only" 或禁用，并确保
**"XHCI Hand-off"** 已启用。有些主板为了缩短开机时间，会在操作系统加载
前跳过完整的 USB 初始化，这会导致*任何*操作系统（不仅仅是这个）在正常
启动流程中要晚得多才能看到鼠标。无论如何，屏幕上的键盘回退方案（方向键 +
Enter）都能正常工作。

### 从零编写的 xHCI 鼠标驱动

UEFI 自身的指针协议在一些真实主板上被证明并不可靠（`EFI_ABSOLUTE_POINTER_
PROTOCOL` 报告说找到了设备，却从来没有一次返回真实的移动数据）。现在
`boot/boot.c` 中有一个原创的 xHCI（USB 3 主机控制器）驱动，完全从零编写，
而不是借用自任何其他项目，它彻底绕开了 UEFI 的 USB 协议栈来处理鼠标：

1. 通过 `EFI_PCI_IO_PROTOCOL` 找到 xHCI 控制器（PCI 类别 0x0C/0x03，
   prog-if 0x30），启用其 PCI 内存解码和总线主控（bus mastering），并
   读取其 64 位 MMIO BAR。屏幕上显示为 `XHCI: FOUND`。
2. 重置并初始化控制器：停止它、执行主机控制器复位、编程 Device Context
   Base Address Array、建立 Command Ring 和轮询式 Event Ring，然后重新
   启动它。屏幕上显示为 `XHCIINIT: OK`。以这种方式接管控制器会重置固件
   自身 USB 协议栈已经建立的一切，所以此时 `EFI_ABSOLUTE_POINTER_
   PROTOCOL` 的支持会消失——这是预期行为，因为这个驱动正是要取代它。
3. 扫描每一个根集线器端口，寻找已连接的设备，必要时发出 Port Reset
   （在 xHCI 上对 USB2 和 USB3 端口都适用）。
4. 启用一个设备槽位，为其分配地址（`Address Device`），并通过默认的
   control 端点获取其配置描述符，从中找到一个 interrupt IN 端点——
   优先选择位于 HID 启动协议（boot-protocol）鼠标接口内的端点，但如果
   没有，也会接受任意 interrupt IN 端点作为后备方案。设置配置，并且
   对 HID 接口切换到 Boot Protocol。
5. 为该 interrupt 端点发出 `Configure Endpoint` 命令，并始终保持一个
   在途的 Normal TRB，直接解码每一份完成的 HID 启动鼠标报告（按键、
   带符号的 dx/dy）——完全不涉及 UEFI 指针协议。屏幕上显示为
   `XHCIMOUSE: ACTIVE`，附带它绑定的端口/槽位/端点，以及 `XHCISTATE`
   显示控制器自身记录的 Slot/Endpoint 状态（一切正常时为
   Configured/Running）。

这套流程已经在 QEMU（`qemu-xhci` + `usb-mouse`）中做了端到端验证：端口
扫描、槽位启用、地址分配、描述符解析、配置以及端点设置全部成功，之后
读回的控制器自身状态也确实是 Slot Configured / Endpoint Running。但在
自动化、无头（headless）的 QEMU 测试中确认*真实的 HID 报告*能完整流通，
却卡在了 QEMU 自身的输入路由问题上（它同时存在三个相互竞争的虚拟指针
设备——PS/2、绝对坐标的 "vmmouse"，以及 USB HID 鼠标，而 monitor 的
`mouse_move`、QMP 的 `input-send-event`，以及 VNC 的指针事件，在那个
测试环境里都无法确定地指向 USB 设备），这并不能归咎于这个驱动本身的
缺陷。在真实硬件上只会有一个物理鼠标，不存在这种歧义——如果鼠标依然
不动，屏幕上的诊断信息（`XHCIMOUSE`、`XHCISTATE`、`XHCIEVT`）会准确
显示初始化进行到了哪一步，这是弄清楚该主板具体差异所在最快的办法。

## 命令行

一个小型终端窗口：点击桌面上的 **CMD** 图标，或 **开始 -> Command
Line** 即可打开它，点击 **EXIT** 或其关闭按钮即可关闭。它打开时拥有
独立的键盘焦点（这样打字就不会同时移动鼠标光标，也不会点击到它下面的
内容），有滚动缓冲区，并内置了几个命令：

| 命令          | 作用 |
|---------------|------|
| `HELP`        | 列出内置命令 |
| `ABOUT`       | 关于 hentrOS |
| `VER`         | 显示版本号 |
| `TIME` / `DATE` | 通过 `EFI_RUNTIME_SERVICES.GetTime` 显示当前时间/日期 |
| `ECHO 文本`   | 把文本原样打印出来 |
| `CLS`         | 清屏 |
| `REBOOT`      | 重启机器 |
| `EXIT`        | 关闭窗口 |

它背后并没有真正的文件系统或进程模型——与这个玩具操作系统其余部分一样，
它只是一组固定的内置命令，而不是可以启动程序的完整 shell。自制的 5x7
位图字体只覆盖 `0-9`、`A-Z` 和少量标点符号（`: . , - ! '`），所以命令
输出也只会用到这个字符集，而不会出现 `< > _ \ [ ]` 这类符号。

## 启动菜单：Live 模式还是安装

在对磁盘做任何操作之前，引导程序总会先显示一个带两个选项的菜单：

- **[1] Live 模式** —— 直接从内存运行 hentrOS，只使用启动所用的介质。
  不会写入任何磁盘。
- **[2] 安装** —— 使用 UEFI Simple File System 协议寻找另一个磁盘卷，
  并把 `BOOTX64.EFI` 复制到该卷上一个新建的 `\EFI\HENTROS\` 目录中。
  它绝不会触碰 `\EFI\BOOT\` 或任何其他已存在的文件，所以该磁盘上已安装
  的 Windows/Linux 会被完整保留——hentrOS 只是成为固件一次性启动菜单
  （例如开机时按 F12/Esc）中可以选择的一个额外条目。复制完成后，它会
  立即启动进入 hentrOS 本身，方便你马上试用。

## 现成的 ISO 镜像

仓库根目录下的 [`hentros.iso`](hentros.iso) 是一个预先构建好、可直接
启动的光盘镜像——无需任何工具链即可试用。它是一个标准的纯 UEFI El
Torito ISO（一个嵌入在 ISO 9660 光盘中的小型 FAT12 EFI 系统分区），
构建命令为：

```sh
make iso
```

**不要只是把 `.iso` 文件拖拽复制到 U 盘上**（拖放 / Ctrl+C-Ctrl+V）——
那样只是把文件的副本放到了 U 盘上，并不会让 U 盘变得可启动。请改用
原始镜像写入工具：

- **Windows：** [Rufus](https://rufus.ie) —— 选择目标驱动器，选中
  `hentros.iso`，分区方案选 **GPT**，目标系统选 **UEFI（非 CSM）**，
  如果它询问，写入模式选 **DD Image**（不是 ISO 模式）。或者使用
  [balenaEtcher](https://etcher.balena.io)，它会自动完成这一切。
- **Linux/macOS：** `dd if=hentros.iso of=/dev/sdX bs=4M status=progress`
  （**请仔细核对设备名**——这会覆盖整个驱动器）。

它也可以直接作为虚拟光驱在支持 UEFI 的虚拟机中使用（VirtualBox、
VMware、QEMU：`qemu-system-x86_64 -bios OVMF.fd -cdrom hentros.iso`）。
无论哪种方式，最终都会进入同一个启动菜单，并且需要先在固件设置中
**关闭 Secure Boot**——这个引导程序没有签名。

## 编译

需要 `clang`+`lld`（针对 `x86_64-unknown-windows` / PE-COFF 目标构建，
这样才能生成 EFI 应用程序）。在 Debian/Ubuntu 上：

```sh
apt-get install clang lld qemu-system-x86 ovmf mtools dosfstools xorriso
make        # 生成 iso/EFI/BOOT/BOOTX64.EFI
make iso    # 同时生成 hentros.iso
```

## 运行

```sh
make run
```

会直接把 `iso/` 目录作为 FAT 卷，在配有 OVMF 固件的 QEMU 中启动，并附带
模拟的 USB 鼠标/键盘，从而走真实的输入路径，而不是 QEMU 默认的 PS/2
设备。移动鼠标即可与桌面交互：点击 **Start** 打开菜单，拖动窗口标题栏
移动窗口，点击红色 **X** 关闭窗口——如果没有可用的指针设备，也可以使用
方向键和 Enter 键。
