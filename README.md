# STL Quick View

Windows x64 的輕量 3D 模型檢視器，支援 STL、3MF、OBJ 和 PLY。可從檔案總管快速查看 STL 外形與 3MF 第一盤縮圖。

## 安裝

在此儲存庫的 **Releases** 頁面下載 Windows x64 的 `.msi`，雙擊安裝。安裝程式會為目前使用者建立開始功能表捷徑，並註冊檔案總管預覽和縮圖支援。

若要變更雙擊開啟的程式或套用專用 STL 圖示，在 Windows「設定 → 應用程式 → 預設應用程式」選擇 **STL Quick View**。

## 使用

- **主程式**：開啟或拖入模型。左鍵拖曳旋轉、右鍵拖曳平移、滾輪縮放。點右上角的視角立方體可切換視角，背景色和模型色可在「設定」調整。
- **STL**：按 `Alt+P` 開啟檔案總管的預覽窗格，選取檔案查看固定角度的模型輪廓。切換到「中圖示」以上也可查看模型縮圖。
- **3MF**：切換到「中圖示」以上可查看檔內既有的第一盤圖片。只有在檔案內含可用圖片時才顯示縮圖，否則沿用一般圖示。

若先前曾以 `Setup.ps1` 註冊檔案總管整合，請先用原腳本加上 `-Uninstall` 移除，再安裝 MSI。已透過 MSI 安裝的舊版可以直接更新。可從 Windows 的「已安裝的應用程式」移除。

## 從原始碼建置

使用 Visual Studio C++ Build Tools 和 Windows SDK，執行 `.\build.ps1`。若要建置 MSI，需另外準備 WiX Toolset 7 和相同版本的 `WixToolset.UI.wixext`，執行 `.\build-msi.ps1`。成品位於 `dist\`。

本專案採用 [MIT 授權](LICENSE)。第三方元件的授權見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
