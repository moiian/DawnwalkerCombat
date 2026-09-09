param(
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Destination
)
$ErrorActionPreference='Stop'
$sourceRoot=(Resolve-Path -LiteralPath $Source).Path.TrimEnd('\')
$destinationRoot=[IO.Path]::GetFullPath($Destination).TrimEnd('\')
if ($destinationRoot.StartsWith($sourceRoot,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'The overlay must be outside the original animation folder'
}
$map=[ordered]@{ Up=@('KDForward',1); Right=@('KDRight',2); Down=@('KDBack',3); Left=@('KDLeft',4); None=@('',0) }
$graphNames=@('KDForward','KDRight','KDBack','KDLeft')
$count=0
foreach ($direction in $map.Keys) {
    foreach ($suffix in @('','-Attacks')) {
        $folder="Sword-$direction$suffix"
        $path=Join-Path $sourceRoot "$folder/config.json"
        $config=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
        $oldConditions=@($config.conditions | Where-Object { $_.'Value A'.graphVariable -in $graphNames })
        $expected=if ($direction -eq 'None') { 4 } else { 1 }
        if ($oldConditions.Count -ne $expected) { throw "Unexpected direction condition count: $folder" }
        foreach ($condition in $oldConditions) {
            $expectedValue=if ($direction -eq 'None') { 0 } else { 1 }
            if ($condition.condition -ne 'CompareValues' -or $condition.Comparison -ne '==' -or
                $condition.'Value A'.graphVariableType -ne 'Bool' -or $condition.'Value B'.value -ne $expectedValue) {
                throw "Unrecognized old direction logic: $folder"
            }
            if ($direction -ne 'None' -and $condition.'Value A'.graphVariable -ne $map[$direction][0]) {
                throw "Mismatched old direction: $folder"
            }
        }
        $otherConditions=@($config.conditions | Where-Object { $_.'Value A'.graphVariable -notin $graphNames })
        $graphVariable=if ($suffix -eq '-Attacks') { 'DW_AttackDirection' } else { 'DW_InputDirection' }
        $config.conditions=@($otherConditions) + @([ordered]@{
            condition='CompareValues'; requiredVersion='1.0.0.0'
            'Value A'=[ordered]@{graphVariable=$graphVariable;graphVariableType='Int'}
            Comparison='=='
            'Value B'=[ordered]@{value=[int]$map[$direction][1]}
        })
        if ($suffix -eq '-Attacks') {
            $config.interruptible=$true
        }
        $relative="meshes/actors/Character/animations/OpenAnimationReplacer/Dawnwalker combat/$folder"
        $target=Join-Path $destinationRoot $relative
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        $targetFile=Join-Path $target 'config.json'
        $config | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $targetFile -Encoding utf8NoBOM
        # Validate all non-direction properties by normalized content, without file digests.
        $original=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable
        $written=Get-Content -LiteralPath $targetFile -Raw | ConvertFrom-Json -AsHashtable
        $original.Remove('conditions'); $written.Remove('conditions')
        if (($original|ConvertTo-Json -Depth 30 -Compress) -ne ($written|ConvertTo-Json -Depth 30 -Compress)) {
            throw "Non-direction property changed in $folder"
        }
        ++$count
    }
}
Write-Output "Created $count OAR config overlays; original files and animation assets unchanged."
