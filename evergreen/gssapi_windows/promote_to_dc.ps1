<#
.SYNOPSIS
  Promote this Windows host into its own new Active Directory forest, so mongod, the KDC, and the
  client all live on one host with no cross-host traffic.

.DESCRIPTION
  Uses -NoRebootOnCompletion so the caller controls the reboot (Install-ADDSForest's auto-reboot
  fires unpredictably and can interrupt the next step). The local Administrator becomes the domain
  Administrator with the same password, so ssh key auth keeps working after the reboot.
#>
param(
    [Parameter(Mandatory = $true)][string]$Realm,
    [Parameter(Mandatory = $true)][string]$SafeModePassword
)

$ErrorActionPreference = "Stop"

Write-Host "Installing AD DS role..."
Install-WindowsFeature -Name AD-Domain-Services -IncludeManagementTools | Out-Host

Import-Module ADDSDeployment
$smpw = ConvertTo-SecureString $SafeModePassword -AsPlainText -Force

# Derive a NetBIOS name from the first label of the realm (e.g. WINGSSAPI.LOCAL -> WINGSSAPI).
$netbios = ($Realm -split '\.')[0].ToUpper()

Write-Host "Promoting to a new forest: $Realm (NetBIOS $netbios). The host will reboot."
Install-ADDSForest `
    -DomainName $Realm `
    -DomainNetbiosName $netbios `
    -SafeModeAdministratorPassword $smpw `
    -InstallDns `
    -NoRebootOnCompletion:$true `
    -Force
