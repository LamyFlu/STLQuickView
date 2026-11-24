# STL Quick View

Windows 10 1607 以上及 Windows 11 的原生 x64 模型檢視器，支援 STL、3MF、OBJ 和 PLY。

從 [GitHub Releases](https://github.com/LamyFlu/STLQuickView/releases/latest) 下載 Windows x64 MSI，雙擊安裝。

## 開啟模型

從開始功能表啟動 **STL Quick View**，拖入模型檔，或按 `Ctrl+O` 選擇檔案。

支援的格式：

- **STL**（ASCII 與二進位）：ASCII 檔若有多個 `solid` 區塊，會各自列為一個物件。
- **3MF**：支援元件、建構項目的變換矩陣，以及 Bambu Studio／PrusaSlicer 使用的多檔結構（production 擴充）。物件名稱取自檔案或 Bambu 的設定檔，尺寸單位採用檔案標示的單位。
- **OBJ**：多邊形面自動分割為三角形，`o`／`g` 分組列為物件。OBJ 慣例為 Y 軸朝上，載入時會轉成與 STL 相同的 Z 軸朝上。
- **PLY**（ASCII 與二進位）。

只載入外形。檔案若帶有顏色、材質或貼圖（3MF 材質與顏色群組、多色塗裝、Bambu 多色耗材、OBJ 材質、PLY 頂點顏色、STL 色彩屬性），畫面左側會顯示「此檔案含有顏色資訊」，底部狀態列也會標示。檔案有多個物件時，左側會列出物件清單：點選物件會像單獨開啟該物件一樣顯示：只畫出該物件、置中並以它的大小重新取景，格線與座標軸也改以該物件為準；點「全部顯示」恢復。底部會顯示所選物件的三角面數與尺寸。

- 按住左鍵拖曳旋轉，可自由轉過頂部與底部；按住右鍵或中鍵拖曳平移，用滾輪縮放。
- 點右上角立方體的可見面切換視角（會平滑轉場），或在立方體上按右鍵選擇六個方向之一。立方體上的文字會隨面一起轉動。
- 按「重設視角」、`Home` 或在模型區按兩下左鍵，可回到初始視角。
- 「設定」可切換座標軸（`A`）與地板格線（`G`），並選擇背景色、模型色（預設淺灰）與尺寸單位，設定會儲存在目前使用者的設定中。曾自訂過顏色的使用者，可按「設定 → 還原預設顏色」改用新的淺灰預設。STL 的檔案總管縮圖固定使用淺藍色。
- 座標軸畫在檔案原點；原點離模型太遠時改畫在模型外框的角落。
- 底部顯示三角面數、X × Y × Z 的範圍與格線間距。STL、OBJ、PLY 沒有標示長度單位，預設以 mm 標示，可在「設定 → 尺寸單位」改為 cm、m、in 或不顯示；3MF 會標示單位，以檔案為準。格線間距依單位調整：公制使用 1、2、5 的倍數，英寸使用 1/64 到 1/2 的分數與 1、2、3、6、12 英寸，並在整英寸或整英尺處加深線條。
- 繪圖預設使用 Direct3D 11（8 倍多重取樣抗鋸齒），無法使用時自動改用 OpenGL；兩者都無法使用硬體加速時，依序改用 Direct3D 軟體繪圖（WARP）、軟體 OpenGL，最後是 CPU 繪圖。可在「設定 → 繪圖引擎」指定 Direct3D 11 或 OpenGL，選單中也會顯示目前使用的顯示卡；底部狀態列會標示目前的引擎。
- 雙顯示卡筆電會直接要求使用獨立顯示卡（Direct3D 透過 DXGI 選擇高效能顯示卡；OpenGL 透過 NVIDIA Optimus／AMD 切換顯示卡設定），MSI 也會在 Windows 圖形設定中將本程式設為高效能。

## 檔案總管整合

執行 `StlQuickView-win-x64.msi`，安裝程式會為目前使用者啟用檔案總管預覽與模型縮圖、註冊專用 STL 圖示，並建立開始功能表捷徑。安裝完成頁可直接開啟 Windows 預設應用程式設定。首次安裝時可在歡迎頁選擇安裝位置（預設為 `%LOCALAPPDATA%\STL Quick View`；這是只套用到目前使用者的安裝，請選擇不需系統管理員權限即可寫入的資料夾）。新版 MSI 可直接覆蓋安裝舊版或相同版本，會沿用原本的安裝位置；不能安裝比現有版本舊的版本。

若先前已用 `Setup.ps1` 安裝，請先用同一支腳本加上 `-Uninstall` 移除舊版整合，再執行 MSI。

若使用 ZIP 版，也可在解壓縮後的資料夾開啟 PowerShell，執行下列指令。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Setup.ps1
```

在檔案總管按 `Alt+P` 開啟預覽窗格，選取 STL、3MF、OBJ 或 PLY 就會看到固定斜上方的模型輪廓，下方標示 X × Y × Z 尺寸：STL、OBJ、PLY 不標單位，3MF 依檔案標示的單位顯示，多物件檔案另外顯示物件數。3MF 的預覽會取代 Windows 內建的 3MF 預覽（只套用到目前使用者，移除後恢復）；若已有其他程式設定的預覽，則保留原設定。預覽與 STL 縮圖最多讀取 64 MiB。大型模型可在主程式開啟：超過 128 MiB 或 200 萬個三角面的檔案，會先顯示三角面數與預估記憶體用量，按「仍要載入」才載入，上限放寬到 2 GiB 或 3,000 萬個三角面。MSI 也會將 3MF、OBJ、PLY 加入「開啟檔案」選單。

若要讓雙擊 STL 時用本程式開啟，並顯示專用 STL 圖示，在 Windows「設定 → 應用程式 → 預設應用程式」搜尋 `.stl`，選取 **STL Quick View**。

在檔案總管切換到「中圖示」以上的檢視方式：

- **STL** 顯示淺藍色模型縮圖。
- **3MF** 顯示檔內的縮圖：依序取用 Bambu Studio／OrcaSlicer 的第一盤圖片（`Metadata/plate_1.png`，小尺寸時改用 `plate_1_small.png`）、3MF 規範中 `_rels/.rels` 指定的封裝縮圖（PrusaSlicer、Cura 等），以及 `Metadata/thumbnail.png`。支援 PNG 與 JPEG，保留原圖色彩和透明度，並自動裁掉透明邊框；圖片不會放大超過原尺寸（最大 1024 px）。只讀取 ZIP 目錄與那一張圖片。檔案沒有內嵌圖片時，改為繪製模型本身（與 STL 相同的淺藍透明縮圖，解壓後超過 256 MiB 的檔案則使用一般圖示）。此功能也適用於已有其他預設開啟程式的情況。

「詳細資料」與「清單」等小圖示檢視通常仍顯示檔案圖示。安裝前已產生的舊縮圖可能留在快取中，可用「磁碟清理」勾選「縮圖」清除。ZIP 版需加上 `Setup.ps1 -Thumbnails` 才會啟用縮圖。

可在 Windows「設定 → 應用程式 → 已安裝的應用程式」移除 MSI 版。ZIP 版可執行 `Setup.ps1 -Uninstall`。若已將本程式設為預設，請先在 Windows 設定選取其他開啟程式。若 Windows 正在使用舊版的預覽元件，可在登出再登入後重跑安裝或移除指令以清理舊檔。

## 建置

安裝 Visual Studio C++ Build Tools 與 Windows 10 或 11 SDK，從原始碼目錄執行 `.\build.ps1`。成品輸出到 `dist`。著色器（`shaders.hlsl`）在建置時由 SDK 的 `fxc` 預先編譯並內嵌到程式中，執行時不需要額外的 DLL。

繪圖程式碼位於 `render.h`（共用介面與場景計算）、`render_d3d11.cpp`（Direct3D 11）與 `render_gl.cpp`（OpenGL 備援）。

建置 MSI 時，另外準備 WiX Toolset 7 與同版的 `WixToolset.UI.wixext`，再執行 `.\build-msi.ps1`。該指令也接受 `-WixPath` 指定 WiX 執行檔。

版本號只記錄在 `VERSION` 檔，建置時會寫入 exe、DLL 與 MSI。發行新版時執行 `.\build-msi.ps1 -Bump Patch`（或 `Minor`、`Major`），會先調高版本號再建置 MSI 與 ZIP。安裝程式的圖片可用 `installer\make-art.ps1` 重新產生。

## 授權

本專案採用 [MIT 授權](LICENSE)。第三方元件的授權見 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
