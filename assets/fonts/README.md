# MiSans

Atrium 的界面字体（D40）。这里是小米 MiSans 的三个字重，未经任何修改，作为 Atrium 的一部分随附：

| 文件 | 字重（OS/2） | 在 Atrium 中的用途 |
|---|---|---|
| `MiSans-Medium.otf` | 380 | 正文（相当于常见字体的 Regular） |
| `MiSans-Demibold.otf` | 450 | 强调、玻璃上的小字 |
| `MiSans-Semibold.otf` | 520 | 标题、大号数字 |

- 版权：小米科技有限责任公司。许可：《MiSans 字体知识产权许可协议》，见 <https://hyperos.mi.com/font/download>。
- 协议要求：在软件中注明使用了 MiSans（设置中心“关于”页）；不得修改字体；不得脱离软件单独分发或出售字体文件。
- 字体文件只由 Atrium 从自己的 `Fonts` 文件夹私有加载，不安装到系统。缺少这些文件时 Atrium 退回 Segoe UI Variable 与系统中文字体。
- 任务栏上的输入法字符（`中`、`あ`、`가`、`A`、`a`）不再随附字体文件，而是作为内置路径轮廓绘制（`native/core/render/ime_glyph_data.h`、`settings/Atrium.Settings/ImeGlyphData.cs`，由 `tools/generate-ime-glyphs.py` 生成），轮廓取自 **Noto Sans CJK SC Regular**（© Google / Adobe，依据 SIL Open Font License 1.1 许可使用，见 <https://scripts.sil.org/OFL>；任何修改或再分发须保留此版权与许可声明，且不得单独出售字体本身）。其余字符以 `MiSans Medium` 绘制。
