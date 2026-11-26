# 从 v2.7.0 及更早版本升级的迁移说明

从本版本起，应用内更新与卸载脚本均改为由安装包清单（`package-manifest.json`）驱动，仅替换或移除官方发行文件，原地保留您在安装目录中自行存放的额外文件。

但如果您当前正在运行 **`v2.7.0` 或更早版本**，应用内自动更新仍会执行**旧安装目录中自带的 `Update.ps1`**。该旧版脚本在更新时会整树移走并清理原安装目录内容，因此若您曾在旧安装目录（包括根目录、`Settings\` 或 `locales\`）存放过自定义文件，请按以下方式完成首次迁移：

1. **不要直接覆盖旧目录或从旧版触发应用内更新**：从发布页下载新版压缩包后，**务必将新版解压到一个全新的空目录**，并保留旧目录不动。
2. **启动新版程序**：运行新目录中的 `Atrium.exe`。
   - 您的个人设置、主题配置与桌面小组件布局保存在 `%LOCALAPPDATA%\Atrium` 中，新版会无缝沿用。
   - 若旧版正在运行，新版会提示是否接管运行，并在启动后自动将 Windows 开机自启项与注册信息更新到新目录。
3. **后续更新与卸载**：完成本次迁入新版目录后，之后的程序内更新与 `Uninstall.ps1` 均会基于 `package-manifest.json` 精准操作官方文件，不会再动到您的个人或自定义文件。

## Upgrading from v2.7.0 or earlier

Starting with this release, the in-app updater and uninstaller are driven by `package-manifest.json`, replacing or removing only official release files while leaving any extra files you placed in the install folder untouched.

If you are currently running **`v2.7.0` or earlier**, an in-app update still runs **the old `Update.ps1` inside the existing folder**, which moves and clears the whole folder during an update. If you keep custom files in that folder (including the root directory, `Settings\`, or `locales\`), migrate once as follows:

1. **Do not overwrite the old folder or trigger an in-app update from the old version**: download the new release archive and **extract it to a brand-new empty folder**, leaving the old folder as-is.
2. **Launch the new build**: run `Atrium.exe` in the new folder.
   - Your personal settings, themes, and desktop widget layouts live in `%LOCALAPPDATA%\Atrium` and carry over automatically.
   - If the old version is running, the new build will ask to take over and automatically update the Windows startup entry and registration to the new folder.
3. **Future updates and uninstalls**: once moved to the new folder, subsequent in-app updates and `Uninstall.ps1` use `package-manifest.json` and will no longer touch your custom files.
