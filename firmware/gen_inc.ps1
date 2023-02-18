$path = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $path

$filename = $path + "\em8rl1.ihx"

$file = New-Object System.IO.StreamReader($filename, [System.Text.Encoding]::GetEncoding("sjis"))
while(($line = $file.ReadLine()) -ne $null)
{
    Write-Output('"' + $line + '",')
}
$file.Close()
