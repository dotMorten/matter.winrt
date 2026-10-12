$ErrorActionPreference = "Stop"
$source = Join-Path $PSScriptRoot "verify-abi.ps1"
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "ABI checker parsing failed."
}
$comparison = $ast.Find({
    param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq "Compare-WinmdDeclarations"
}, $false)
if ($null -eq $comparison) {
    throw "ABI comparison function was not found."
}
. ([scriptblock]::Create($comparison.Extent.Text))

$enumKey = "enum Test.Stage"
$interfaceKey = "interface Test.IController"
$baseline = @{
    $enumKey = @("enum Stage", "{", "Validation = 0,", "Capabilities = 8", "};")
    $interfaceKey = @("[uuid(original)]", "interface IController", "{", "HRESULT Read();", "};")
}
$current = @{
    $enumKey = @("enum Stage", "{", "Validation = 0,", "Capabilities = 8,", "Allocation = 9", "};")
    $interfaceKey = $baseline[$interfaceKey]
}
if (@(Compare-WinmdDeclarations $baseline $current).Count -ne 0) {
    throw "An additive enum member was incorrectly treated as an ABI break."
}
$current[$enumKey] = @("enum Stage", "{", "Validation = 0,", "Capabilities = 9,", "Allocation = 10", "};")
if (@(Compare-WinmdDeclarations $baseline $current).Count -eq 0) {
    throw "A changed enum value was accepted."
}
$current[$enumKey] = @("enum Stage", "{", "Validation = 0", "};")
if (@(Compare-WinmdDeclarations $baseline $current).Count -eq 0) {
    throw "A removed enum member was accepted."
}
$current[$enumKey] = $baseline[$enumKey]
$current[$interfaceKey] = @("[uuid(original)]", "interface IController", "{", "HRESULT Read();", "HRESULT Write();", "};")
if (@(Compare-WinmdDeclarations $baseline $current).Count -eq 0) {
    throw "An existing interface vtable change was accepted."
}
$current[$interfaceKey] = @("[uuid(replaced)]", "interface IController", "{", "HRESULT Read();", "};")
if (@(Compare-WinmdDeclarations $baseline $current).Count -eq 0) {
    throw "A changed interface identity was accepted."
}
Write-Host "PASS additive enum formatting, removed/changed enum values, interface identity and vtable preservation."
