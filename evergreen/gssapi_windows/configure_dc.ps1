<#
.SYNOPSIS
  Once the DC is back up after promotion, register the mongod service SPN and create the client
  test principal.

.DESCRIPTION
  mongod runs as SYSTEM (the machine account), so the service SPN is registered on that account.
  The client authenticates as a dedicated user with an explicit password (created here), because
  the test's public-key ssh session holds no Kerberos credentials of its own.
#>
param(
    [Parameter(Mandatory = $true)][string]$Realm,
    [Parameter(Mandatory = $true)][string]$ServiceName,
    [Parameter(Mandatory = $true)][string]$ServiceHostname,
    [Parameter(Mandatory = $true)][string]$UserName,
    [Parameter(Mandatory = $true)][string]$UserPassword
)

$ErrorActionPreference = "Stop"
# The caller detects success by grepping for the final "DC configured:" line (exit codes do not
# survive the ssh -> cmd -> powershell chain), so ensure any failure stops before printing it.
trap { Write-Host "CONFIGURE_DC FAILED: $_"; exit 1 }

# Wait for AD DS / AD Web Services to be fully up after the promotion reboot.
$ready = $false
foreach ($attempt in 1..30) {
    try {
        Import-Module ActiveDirectory -ErrorAction Stop
        Get-ADDomain -ErrorAction Stop | Out-Null
        $ready = $true
        break
    } catch {
        Write-Host "Waiting for AD DS to come up (attempt $attempt): $($_.Exception.Message)"
        Start-Sleep -Seconds 10
    }
}
if (-not $ready) { throw "AD DS did not become ready after promotion" }

Write-Host "=== AD domain info ==="
Get-ADDomain | Format-List Forest, DNSRoot, NetBIOSName, DomainMode | Out-Host

# Register the service SPN on the machine account, matching the test's --gssapiHostName.
setspn -S "$ServiceName/$ServiceHostname" "$env:COMPUTERNAME`$" | Out-Host

Write-Host "=== SPNs on $env:COMPUTERNAME`$ ==="
setspn -L "$env:COMPUTERNAME`$" | Out-Host

# Create the client test principal (authenticated by explicit password; see the file header).
Write-Host "Creating test user $UserName@$Realm..."
$pw = ConvertTo-SecureString $UserPassword -AsPlainText -Force
New-ADUser -Name $UserName `
    -SamAccountName $UserName `
    -UserPrincipalName "$UserName@$Realm" `
    -AccountPassword $pw `
    -Enabled $true `
    -PasswordNeverExpires $true
Get-ADUser $UserName | Format-List Name, UserPrincipalName, Enabled | Out-Host

# Validate the credentials with an LDAP bind, so a bad password fails here with a clear message
# rather than as an opaque SEC_E_LOGON_DENIED in the client later.
$bind = New-Object System.DirectoryServices.DirectoryEntry("LDAP://localhost", "$UserName@$Realm", $UserPassword)
if (-not $bind.distinguishedName) {
    throw "LDAP bind as $UserName@$Realm with the provided password failed"
}
Write-Host "Validated $UserName@$Realm credentials via LDAP bind."

Write-Host "DC configured: realm=$Realm service=$ServiceName/$ServiceHostname user=$UserName@$Realm"
