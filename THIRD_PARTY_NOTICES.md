# 第三方元件聲明

## WiX Toolset 7.0.0 與 WixToolset.UI.wixext

MSI 使用 WiX Toolset 建置，包含 WixUI 安裝精靈的標準對話框和部分介面資源。WiX 原始碼和適用的上游授權請參閱以下資料。

- [.NET Foundation 與貢獻者的 WiX 授權（Microsoft Reciprocal License）](https://github.com/wixtoolset/wix/blob/v7.0.0/LICENSE.TXT)
- [WiX 7.0.0 原始碼](https://github.com/wixtoolset/wix/tree/v7.0.0)
- [WiX 7.0.0 Open Source Maintenance Fee 條款](https://github.com/wixtoolset/wix/blob/v7.0.0/OSMFEULA.txt)

## Microsoft Visual C++ 與 Windows SDK

主程式與 DLL 以 Visual C++ 建置並靜態連結 C/C++ 執行階段。Windows SDK 用於編譯和連結 Windows API，以及編譯專案的 HLSL 著色器。相關元件適用 Microsoft 的授權和再散布條款。

- [Visual Studio 2026 可再散布元件](https://learn.microsoft.com/visualstudio/releases/2026/redistribution)
- [Microsoft Visual C++ 再散布授權](https://visualstudio.microsoft.com/license-terms/vs2026-ga-visualcpp-v14-redist-runtime/)

## WiX 授權全文

```text
Copyright (c) .NET Foundation and contributors.
This software is released under the Microsoft Reciprocal License (MS-RL) (the "License"); you may not use the software except in compliance with the License.

The text of the Microsoft Reciprocal License (MS-RL) can be found online at:
 http://opensource.org/licenses/ms-rl


Microsoft Reciprocal License (MS-RL)

This license governs use of the accompanying software. If you use the software, you accept this license. If you do not accept the license, do not use the software.

1. Definitions
 The terms "reproduce," "reproduction," "derivative works," and "distribution" have the same meaning here as under U.S. copyright law.
 A "contribution" is the original software, or any additions or changes to the software.
 A "contributor" is any person that distributes its contribution under this license.
 "Licensed patents" are a contributor's patent claims that read directly on its contribution.

2. Grant of Rights
 (A) Copyright Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free copyright license to reproduce its contribution, prepare derivative works of its contribution, and distribute its contribution or any derivative works that you create.
 (B) Patent Grant- Subject to the terms of this license, including the license conditions and limitations in section 3, each contributor grants you a non-exclusive, worldwide, royalty-free license under its licensed patents to make, have made, use, sell, offer for sale, import, and/or otherwise dispose of its contribution in the software or derivative works of the contribution in the software.

3. Conditions and Limitations
 (A) Reciprocal Grants- For any file you distribute that contains code from the software (in source code or binary format), you must provide recipients the source code to that file along with a copy of this license, which license will govern that file. You may license other files that are entirely your own work and do not contain code from the software under any terms you choose.
 (B) No Trademark License- This license does not grant you rights to use any contributors' name, logo, or trademarks.
 (C) If you bring a patent claim against any contributor over patents that you claim are infringed by the software, your patent license from such contributor to the software ends automatically.
 (D) If you distribute any portion of the software, you must retain all copyright, patent, trademark, and attribution notices that are present in the software.
 (E) If you distribute any portion of the software in source code form, you may do so only under this license by including a complete copy of this license with your distribution. If you distribute any portion of the software in compiled or object code form, you may only do so under a license that complies with this license.
 (F) The software is licensed "as-is." You bear the risk of using it. The contributors give no express warranties, guarantees or conditions. You may have additional consumer rights under your local laws which this license cannot change. To the extent permitted under your local laws, the contributors exclude the implied warranties of merchantability, fitness for a particular purpose and non-infringement.
```
