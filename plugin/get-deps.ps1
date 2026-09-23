# Fetches the WebView2 SDK (a NuGet package) into plugin\deps, where CMakeLists.txt expects it.
$ver  = '1.0.4191.47'
$dest = Join-Path $PSScriptRoot "deps\Microsoft.Web.WebView2.$ver"
if (Test-Path $dest) { "Already there: $dest"; return }
New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
$zip = Join-Path $env:TEMP "microsoft.web.webview2.$ver.zip"
Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$ver/microsoft.web.webview2.$ver.nupkg" -OutFile $zip
Expand-Archive $zip -DestinationPath $dest -Force
Remove-Item $zip
"WebView2 SDK $ver -> $dest"
