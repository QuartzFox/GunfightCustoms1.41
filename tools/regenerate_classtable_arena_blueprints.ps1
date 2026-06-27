param(
    [string]$GameRoot = "C:\Games\COD\Modern Warfare_1.41.2.10056273\Call of Duty Modern Warfare (1.41.2.10056273)\.iw8-mod"
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName Microsoft.VisualBasic

$assetMp = Join-Path $GameRoot "assets\mp"
$dump = Join-Path $GameRoot "asset_dumper\string_table"
$gunsmithDir = Join-Path $dump "mp\gunsmith"
$weaponIdsPath = Join-Path $dump "loot\weapon_ids.csv"
$statstablePath = Join-Path $dump "mp\statstable.csv"
$attachmentMapPath = Join-Path $dump "mp\attachmentmap.csv"
$outputPath = Join-Path $assetMp "classtable_arena_blueprints.csv"

$blockedVariants = @{
    "iw8_ar_tango21" = @(1)
    "iw8_ar_mike4" = @(5)
    "iw8_ar_kilo433" = @(3)
    "iw8_ar_scharlie" = @(3)
    "iw8_sm_uzulu" = @(4)
    "iw8_sh_romeo870" = @(5)
    "iw8_sh_dpapa12" = @(3)
    "iw8_lm_mgolf34" = @(4)
    "iw8_sn_kilo98" = @(16)
    "iw8_sn_alpha50" = @(2)
    "iw8_sn_hdromeo" = @(4)
    "iw8_pi_golf21" = @(3)
    "iw8_pi_cpapa" = @(15)
}

function Add-UniqueString([System.Collections.Generic.List[string]]$list, [System.Collections.Generic.HashSet[string]]$set, [string]$value) {
    $value = ($value ?? "").Trim()
    if ($value.Length -eq 0) { return }
    if ($set.Add($value.ToLowerInvariant())) {
        $list.Add($value)
    }
}

function Get-RootStem([string]$root) {
    return ($root -replace "^iw8_", "")
}

function Get-VariantPath([string]$root) {
    return Join-Path $gunsmithDir ((Get-RootStem $root) + "_variants.csv")
}

function CsvEscape([string]$value) {
    if ($null -eq $value) { $value = "" }
    if ($value.IndexOfAny([char[]]@(',', '"', "`r", "`n")) -ge 0) {
        return '"' + ($value -replace '"', '""') + '"'
    }
    return $value
}

function To-CsvLine([string[]]$cells) {
    return (($cells | ForEach-Object { CsvEscape $_ }) -join ",")
}

function Read-CsvRows([string]$path) {
    $parser = [Microsoft.VisualBasic.FileIO.TextFieldParser]::new($path)
    $parser.SetDelimiters(",")
    $parser.HasFieldsEnclosedInQuotes = $true

    $rows = New-Object System.Collections.Generic.List[object]
    while (!$parser.EndOfData) {
        $rows.Add($parser.ReadFields())
    }
    $parser.Close()
    return $rows
}

function Get-Thermal2SniperRoots([string]$path) {
    $roots = New-Object System.Collections.Generic.HashSet[string]
    if (!(Test-Path -LiteralPath $path)) { return $roots }

    $rows = Read-CsvRows $path
    if ($rows.Count -lt 3) { return $roots }

    $labels = $rows[1]
    $thermal2Col = -1
    for ($i = 0; $i -lt $labels.Count; $i++) {
        if ($labels[$i] -eq "thermal2") {
            $thermal2Col = $i
            break
        }
    }
    if ($thermal2Col -lt 0) { return $roots }

    for ($i = 2; $i -lt $rows.Count; $i++) {
        $cells = $rows[$i]
        if ($cells.Count -le $thermal2Col) { continue }

        $root = ($cells[0] ?? "").Trim()
        $thermal2 = ($cells[$thermal2Col] ?? "").Trim()
        if ($root.StartsWith("iw8_sn_") -and $thermal2.Length -gt 0) {
            [void]$roots.Add($root.ToLowerInvariant())
        }
    }

    return $roots
}

function Get-DisplayNameFromStringRef([string]$root, [string]$stringRef) {
    $stringRef = ($stringRef ?? "").Trim()
    if ($stringRef.Length -gt 0) {
        $parts = $stringRef -split "/"
        $name = $parts[$parts.Count - 1].Trim()
        if ($name.Length -gt 0) { return $name }
    }
    return (Get-RootStem $root).ToUpperInvariant()
}

function Get-ColumnProperty($row, [string[]]$candidates, [int]$fallbackIndex) {
    $props = @($row.PSObject.Properties.Name)
    foreach ($candidate in $candidates) {
        if ($props -contains $candidate) { return $candidate }
    }
    if ($fallbackIndex -ge 0 -and $fallbackIndex -lt $props.Count) { return $props[$fallbackIndex] }
    return $null
}

function Get-CurrentAttachmentMap([string]$path) {
    $result = @{}
    if (!(Test-Path -LiteralPath $path)) { return $result }

    $rows = @(Import-Csv -LiteralPath $path)
    if ($rows.Count -eq 0) { return $result }
    $headers = @($rows[0].PSObject.Properties.Name)

    $byLabel = @{}
    foreach ($row in $rows) {
        $label = $row."<column 0>"
        if ($label) { $byLabel[$label] = $row }
    }
    if (!$byLabel.ContainsKey("loadoutPrimary")) { return $result }

    for ($col = 1; $col -lt $headers.Count; $col++) {
        $header = $headers[$col]
        $root = ($byLabel["loadoutPrimary"].$header ?? "").Trim()
        if ($root.Length -eq 0 -or $root -eq "none") { continue }

        $attachments = New-Object string[] 5
        $hasAttachment = $false
        for ($i = 1; $i -le 5; $i++) {
            $label = "loadoutPrimaryAttachment$i"
            $value = "none"
            if ($byLabel.ContainsKey($label)) {
                $value = ($byLabel[$label].$header ?? "").Trim()
                if ($value.Length -eq 0) { $value = "none" }
            }
            $attachments[$i - 1] = $value
            $lower = $value.ToLowerInvariant()
            if ($lower -ne "none" -and $lower -ne "null") { $hasAttachment = $true }
        }

        $key = $root.ToLowerInvariant()
        if ($hasAttachment -and !$result.ContainsKey($key)) {
            $result[$key] = $attachments
        }
    }

    return $result
}

function Get-VariantNameToId([string]$root) {
    $map = @{}
    $path = Get-VariantPath $root
    if (!(Test-Path -LiteralPath $path)) { return $map }

    foreach ($row in @(Import-Csv -LiteralPath $path)) {
        $variantRef = ($row."<column 1>" ?? "").Trim()
        $variantId = ($row."<column 0>" ?? "").Trim()
        if ($variantRef.Length -eq 0 -or $variantId -notmatch "^\d+$") { continue }
        if (!$map.ContainsKey($variantRef)) {
            $map[$variantRef] = [int]$variantId
        }
    }

    return $map
}

function Get-VariantIdsForRoot([string]$root, [hashtable]$weaponRowsByRoot) {
    $ids = New-Object System.Collections.Generic.SortedSet[int]
    [void]$ids.Add(0)

    $variantNameToId = Get-VariantNameToId $root
    if ($variantNameToId.Count -eq 0 -or !$weaponRowsByRoot.ContainsKey($root)) {
        return "0"
    }

    $blocked = @()
    if ($blockedVariants.ContainsKey($root)) { $blocked = $blockedVariants[$root] }

    foreach ($row in $weaponRowsByRoot[$root]) {
        $variantRef = ($row.variantRef ?? "").Trim()
        if ($variantRef.Length -eq 0 -or !$variantNameToId.ContainsKey($variantRef)) { continue }

        $id = [int]$variantNameToId[$variantRef]
        if ($id -ne 0) {
            $license = 0
            [void][int]::TryParse(($row.license ?? "0"), [ref]$license)
            if ($license -eq 99) { continue }
            if ($blocked -contains $id) { continue }
        }

        [void]$ids.Add($id)
    }

    return (($ids | ForEach-Object { $_.ToString() }) -join " ")
}

function Get-VariantZeroNameFromGunsmith([string]$root) {
    $path = Get-VariantPath $root
    if (!(Test-Path -LiteralPath $path)) { return (Get-RootStem $root).ToUpperInvariant() }

    $row = @(Import-Csv -LiteralPath $path | Select-Object -First 1)
    if ($row.Count -eq 0) { return (Get-RootStem $root).ToUpperInvariant() }
    foreach ($prop in @("<column 17>", "<column 16>")) {
        if ($row[0].PSObject.Properties.Name -contains $prop) {
            return Get-DisplayNameFromStringRef $root $row[0].$prop
        }
    }
    return (Get-RootStem $root).ToUpperInvariant()
}

$currentAttachments = Get-CurrentAttachmentMap $outputPath
$thermal2SniperRoots = Get-Thermal2SniperRoots $attachmentMapPath
$weaponRowsByRoot = @{}
foreach ($row in @(Import-Csv -LiteralPath $weaponIdsPath)) {
    $root = ($row.baseRef ?? "").Trim()
    if ($root.Length -eq 0) { continue }
    if (!$weaponRowsByRoot.ContainsKey($root)) {
        $weaponRowsByRoot[$root] = New-Object System.Collections.Generic.List[object]
    }
    $weaponRowsByRoot[$root].Add($row)
}

$statRoots = New-Object System.Collections.Generic.HashSet[string]
foreach ($row in @(Import-Csv -LiteralPath $statstablePath)) {
    $root = ($row.ref ?? "").Trim()
    if ($root.StartsWith("iw8_")) { [void]$statRoots.Add($root.ToLowerInvariant()) }
}

$roots = New-Object System.Collections.Generic.List[string]
$rootSet = New-Object System.Collections.Generic.HashSet[string]
$displayByRoot = @{}

foreach ($file in @("arenaggweapons.csv")) {
    $path = Join-Path $assetMp $file
    if (!(Test-Path -LiteralPath $path)) { continue }
    foreach ($row in @(Import-Csv -LiteralPath $path)) {
        $rootProp = Get-ColumnProperty $row @("weaponName", "<column 0>") 0
        $nameProp = Get-ColumnProperty $row @("stringRef", "<column 2>") 2
        if (!$rootProp) { continue }

        $root = ($row.$rootProp ?? "").Trim()
        if (!$root.StartsWith("iw8_")) { continue }
        Add-UniqueString $roots $rootSet $root
        if (!$displayByRoot.ContainsKey($root)) {
            $displayByRoot[$root] = Get-DisplayNameFromStringRef $root ($row.$nameProp)
        }
    }
}

foreach ($file in @(Get-ChildItem -LiteralPath $gunsmithDir -Filter "*_variants.csv" | Sort-Object Name)) {
    $root = "iw8_" + ($file.BaseName -replace "_variants$", "")
    $rootKey = $root.ToLowerInvariant()
    if (!$statRoots.Contains($rootKey)) { continue }
    if (!$weaponRowsByRoot.ContainsKey($root)) { continue }
    Add-UniqueString $roots $rootSet $root
    if (!$displayByRoot.ContainsKey($root)) {
        $displayByRoot[$root] = Get-VariantZeroNameFromGunsmith $root
    }
}

foreach ($root in $roots) {
    $variantPath = Get-VariantPath $root
    if (Test-Path -LiteralPath $variantPath) {
        $displayByRoot[$root] = Get-VariantZeroNameFromGunsmith $root
    }
}

$seedBlueprintPath = Join-Path $assetMp "classtable_arena_blueprints.csv.bak_20260625_205435"
$columns = New-Object System.Collections.Generic.List[object]
$columnRootSet = New-Object System.Collections.Generic.HashSet[string]

if (Test-Path -LiteralPath $seedBlueprintPath) {
    $seedRows = @(Import-Csv -LiteralPath $seedBlueprintPath)
    if ($seedRows.Count -gt 0) {
        $seedHeaders = @($seedRows[0].PSObject.Properties.Name)
        $seedByLabel = @{}
        foreach ($row in $seedRows) {
            $label = ($row."<column 0>" ?? "").Trim()
            if ($label.Length -gt 0 -and !$seedByLabel.ContainsKey($label)) {
                $seedByLabel[$label] = $row
            }
        }

        if ($seedByLabel.ContainsKey("loadoutPrimary")) {
            for ($i = 1; $i -lt $seedHeaders.Count; $i++) {
                $header = $seedHeaders[$i]
                $root = ($seedByLabel["loadoutPrimary"].$header ?? "").Trim()
                if (!$root.StartsWith("iw8_")) { continue }

                $seedValues = @{}
                foreach ($row in $seedRows) {
                    $label = ($row."<column 0>" ?? "").Trim()
                    if ($label.Length -gt 0) {
                        $seedValues[$label] = ($row.$header ?? "")
                    }
                }

                $columns.Add([pscustomobject]@{
                    Root = $root
                    SeedValues = $seedValues
                })
                [void]$columnRootSet.Add($root.ToLowerInvariant())
            }
        }
    }
}

foreach ($root in $roots) {
    if ($columnRootSet.Contains($root.ToLowerInvariant())) { continue }
    $columns.Add([pscustomobject]@{
        Root = $root
        SeedValues = $null
    })
    [void]$columnRootSet.Add($root.ToLowerInvariant())
}

$rowLabels = @(
    "loadoutName",
    "loadoutPrimaryAddBlueprintAttachments",
    "loadoutPrimary",
    "loadoutPrimaryAttachment1",
    "loadoutPrimaryAttachment2",
    "loadoutPrimaryAttachment3",
    "loadoutPrimaryAttachment4",
    "loadoutPrimaryAttachment5",
    "loadoutPrimaryCamo",
    "loadoutPrimaryReticle",
    "loadoutPrimaryVariantID",
    "loadoutSecondaryAddBlueprintAttachments",
    "loadoutSecondary",
    "loadoutSecondaryAttachment1",
    "loadoutSecondaryAttachment2",
    "loadoutSecondaryAttachment3",
    "loadoutSecondaryAttachment4",
    "loadoutSecondaryAttachment5",
    "loadoutSecondaryCamo",
    "loadoutSecondaryReticle",
    "loadoutSecondaryVariantID",
    "loadoutEquipmentPrimary",
    "loadoutExtraPowerPrimary",
    "loadoutEquipmentSecondary",
    "loadoutExtraPowerSecondary",
    "loadoutPerk1",
    "loadoutPerk2",
    "loadoutPerk3",
    "loadoutExtraPerk1",
    "loadoutExtraPerk2",
    "loadoutExtraPerk3",
    "loadoutOverkill"
)

$headers = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -le $columns.Count; $i++) {
    $headers.Add("<column $i>")
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add((To-CsvLine $headers.ToArray()))

foreach ($label in $rowLabels) {
    $cells = New-Object System.Collections.Generic.List[string]
    $cells.Add($label)

    foreach ($column in $columns) {
        $root = $column.Root
        $rootKey = $root.ToLowerInvariant()
        $attachments = $null
        if ($currentAttachments.ContainsKey($rootKey)) { $attachments = $currentAttachments[$rootKey] }
        $seedValue = $null
        if ($null -ne $column.SeedValues -and $column.SeedValues.ContainsKey($label)) {
            $seedValue = $column.SeedValues[$label]
        }

        $value = switch ($label) {
            "loadoutName" { if ($seedValue) { $seedValue } else { $displayByRoot[$root] }; break }
            "loadoutPrimaryAddBlueprintAttachments" { "1"; break }
            "loadoutPrimary" { $root; break }
            "loadoutPrimaryAttachment1" { if ($thermal2SniperRoots.Contains($rootKey)) { "thermal2" } elseif ($seedValue) { $seedValue } elseif ($attachments) { $attachments[0] } else { "none" }; break }
            "loadoutPrimaryAttachment2" { if ($thermal2SniperRoots.Contains($rootKey)) { "none" } elseif ($seedValue) { $seedValue } elseif ($attachments) { $attachments[1] } else { "none" }; break }
            "loadoutPrimaryAttachment3" { if ($thermal2SniperRoots.Contains($rootKey)) { "none" } elseif ($seedValue) { $seedValue } elseif ($attachments) { $attachments[2] } else { "none" }; break }
            "loadoutPrimaryAttachment4" { if ($thermal2SniperRoots.Contains($rootKey)) { "none" } elseif ($seedValue) { $seedValue } elseif ($attachments) { $attachments[3] } else { "none" }; break }
            "loadoutPrimaryAttachment5" { if ($thermal2SniperRoots.Contains($rootKey)) { "none" } elseif ($seedValue) { $seedValue } elseif ($attachments) { $attachments[4] } else { "none" }; break }
            "loadoutPrimaryCamo" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutPrimaryReticle" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutPrimaryVariantID" { Get-VariantIdsForRoot $root $weaponRowsByRoot; break }
            "loadoutSecondaryAddBlueprintAttachments" { if ($seedValue) { $seedValue } else { "0" }; break }
            "loadoutSecondary" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryAttachment1" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryAttachment2" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryAttachment3" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryAttachment4" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryAttachment5" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryCamo" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryReticle" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutSecondaryVariantID" { if ($seedValue) { $seedValue } else { "-1" }; break }
            "loadoutEquipmentPrimary" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutExtraPowerPrimary" { if ($seedValue) { $seedValue } else { "FALSE" }; break }
            "loadoutEquipmentSecondary" { if ($seedValue) { $seedValue } else { "none" }; break }
            "loadoutExtraPowerSecondary" { if ($seedValue) { $seedValue } else { "FALSE" }; break }
            "loadoutPerk1" { if ($seedValue) { $seedValue } else { "specialty_quickfix" }; break }
            "loadoutPerk2" { if ($seedValue) { $seedValue } else { "specialty_hardline" }; break }
            "loadoutPerk3" { if ($seedValue) { $seedValue } else { "specialty_amp" }; break }
            "loadoutExtraPerk1" { if ($seedValue) { $seedValue } else { "specialty_null" }; break }
            "loadoutExtraPerk2" { if ($seedValue) { $seedValue } else { "specialty_null" }; break }
            "loadoutExtraPerk3" { if ($seedValue) { $seedValue } else { "specialty_null" }; break }
            "loadoutOverkill" { if ($seedValue) { $seedValue } else { "0" }; break }
            default { "none"; break }
        }

        $cells.Add($value)
    }

    $lines.Add((To-CsvLine $cells.ToArray()))
}

$backupPath = "$outputPath.bak_$(Get-Date -Format yyyyMMdd_HHmmss)"
if (Test-Path -LiteralPath $outputPath) {
    Copy-Item -LiteralPath $outputPath -Destination $backupPath
}

[System.IO.File]::WriteAllLines($outputPath, $lines, [System.Text.UTF8Encoding]::new($false))

$columnRoots = @($columns | ForEach-Object { $_.Root })
$t9Count = @($columnRoots | Where-Object { $_ -like "iw8_*_t9*" }).Count
$thermalSniperColumnCount = @($columnRoots | Where-Object { $thermal2SniperRoots.Contains($_.ToLowerInvariant()) }).Count
$attachmentRoots = $currentAttachments.Count
Write-Output "Wrote $outputPath"
Write-Output "Columns: $($columns.Count) weapons/blueprints + label column"
Write-Output "Unique roots: $(@($columnRoots | Sort-Object -Unique).Count)"
Write-Output "T9 roots: $t9Count"
Write-Output "Thermal sniper columns: $thermalSniperColumnCount"
Write-Output "Preserved attachment recipes for roots: $attachmentRoots"
if ($backupPath) { Write-Output "Backup: $backupPath" }
