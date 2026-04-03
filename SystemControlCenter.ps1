# System Control Center - PowerShell GUI Application
# Requires: PowerShell 5.1+ and .NET Framework
# Run as Administrator for full functionality

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# Create the main form
$form = New-Object System.Windows.Forms.Form
$form.Text = "System Control Center"
$form.Size = New-Object System.Drawing.Size(1000, 700)
$form.StartPosition = "CenterScreen"
$form.Font = New-Object System.Drawing.Font("Segoe UI", 9)

# Create TabControl
$tabControl = New-Object System.Windows.Forms.TabControl
$tabControl.Size = New-Object System.Drawing.Size(960, 600)
$tabControl.Location = New-Object System.Drawing.Point(10, 10)

# === TAB 1: System Info ===
$tabInfo = New-Object System.Windows.Forms.TabPage
$tabInfo.Text = "System Info"
$tabControl.TabPages.Add($tabInfo)

$lblInfo = New-Object System.Windows.Forms.Label
$lblInfo.AutoSize = $true
$lblInfo.Location = New-Object System.Drawing.Point(10, 10)
$lblInfo.Text = "System Information:"
$tabInfo.Controls.Add($lblInfo)

$txtInfo = New-Object System.Windows.Forms.TextBox
$txtInfo.Multiline = $true
$txtInfo.ScrollBars = "Vertical"
$txtInfo.Size = New-Object System.Drawing.Size(920, 500)
$txtInfo.Location = New-Object System.Drawing.Point(10, 40)
$txtInfo.ReadOnly = $true
$tabInfo.Controls.Add($txtInfo)

$btnRefreshInfo = New-Object System.Windows.Forms.Button
$btnRefreshInfo.Text = "Refresh Info"
$btnRefreshInfo.Size = New-Object System.Drawing.Size(100, 30)
$btnRefreshInfo.Location = New-Object System.Drawing.Point(10, 550)
$tabInfo.Controls.Add($btnRefreshInfo)

# === TAB 2: Processes ===
$tabProcesses = New-Object System.Windows.Forms.TabPage
$tabProcesses.Text = "Processes"
$tabControl.TabPages.Add($tabProcesses)

$lstProcesses = New-Object System.Windows.Forms.ListBox
$lstProcesses.Size = New-Object System.Drawing.Size(700, 500)
$lstProcesses.Location = New-Object System.Drawing.Point(10, 10)
$tabProcesses.Controls.Add($lstProcesses)

$btnRefreshProc = New-Object System.Windows.Forms.Button
$btnRefreshProc.Text = "Refresh"
$btnRefreshProc.Size = New-Object System.Drawing.Size(80, 30)
$btnRefreshProc.Location = New-Object System.Drawing.Point(10, 550)
$tabProcesses.Controls.Add($btnRefreshProc)

$btnKillProc = New-Object System.Windows.Forms.Button
$btnKillProc.Text = "End Process"
$btnKillProc.Size = New-Object System.Drawing.Size(100, 30)
$btnKillProc.Location = New-Object System.Drawing.Point(100, 550)
$tabProcesses.Controls.Add($btnKillProc)

$lblProcDetail = New-Object System.Windows.Forms.Label
$lblProcDetail.AutoSize = $true
$lblProcDetail.Location = New-Object System.Drawing.Point(720, 10)
$lblProcDetail.Width = 200
$tabProcesses.Controls.Add($lblProcDetail)

# === TAB 3: Services ===
$tabServices = New-Object System.Windows.Forms.TabPage
$tabServices.Text = "Services"
$tabControl.TabPages.Add($tabServices)

$lstServices = New-Object System.Windows.Forms.ListBox
$lstServices.Size = New-Object System.Drawing.Size(700, 500)
$lstServices.Location = New-Object System.Drawing.Point(10, 10)
$tabServices.Controls.Add($lstServices)

$btnRefreshServ = New-Object System.Windows.Forms.Button
$btnRefreshServ.Text = "Refresh"
$btnRefreshServ.Size = New-Object System.Drawing.Size(80, 30)
$btnRefreshServ.Location = New-Object System.Drawing.Point(10, 550)
$tabServices.Controls.Add($btnRefreshServ)

$btnStartServ = New-Object System.Windows.Forms.Button
$btnStartServ.Text = "Start"
$btnStartServ.Size = New-Object System.Drawing.Size(70, 30)
$btnStartServ.Location = New-Object System.Drawing.Point(100, 550)
$tabServices.Controls.Add($btnStartServ)

$btnStopServ = New-Object System.Windows.Forms.Button
$btnStopServ.Text = "Stop"
$btnStopServ.Size = New-Object System.Drawing.Size(70, 30)
$btnStopServ.Location = New-Object System.Drawing.Point(180, 550)
$tabServices.Controls.Add($btnStopServ)

$btnRestartServ = New-Object System.Windows.Forms.Button
$btnRestartServ.Text = "Restart"
$btnRestartServ.Size = New-Object System.Drawing.Size(70, 30)
$btnRestartServ.Location = New-Object System.Drawing.Point(260, 550)
$tabServices.Controls.Add($btnRestartServ)

# === TAB 4: Event Log ===
$tabEvents = New-Object System.Windows.Forms.TabPage
$tabEvents.Text = "Event Log"
$tabControl.TabPages.Add($tabEvents)

$lstEvents = New-Object System.Windows.Forms.ListBox
$lstEvents.Size = New-Object System.Drawing.Size(920, 500)
$lstEvents.Location = New-Object System.Drawing.Point(10, 10)
$tabEvents.Controls.Add($lstEvents)

$btnRefreshEvt = New-Object System.Windows.Forms.Button
$btnRefreshEvt.Text = "Refresh Errors"
$btnRefreshEvt.Size = New-Object System.Drawing.Size(120, 30)
$btnRefreshEvt.Location = New-Object System.Drawing.Point(10, 550)
$tabEvents.Controls.Add($btnRefreshEvt)

# === Functions ===

function Get-SystemInfo {
    $os = Get-WmiObject Win32_OperatingSystem
    $cpu = Get-WmiObject Win32_Processor
    $ram = Get-WmiObject Win32_PhysicalMemory
    $disk = Get-WmiObject Win32_LogicalDisk -Filter "DriveType=3"
    
    $info = @"
Operating System: $($os.Caption)
Version: $($os.Version)
Architecture: $($os.OSArchitecture)
Uptime: $((New-TimeSpan -Seconds $os.LocalDateTime.Subtract($os.LastBootUpTime).TotalSeconds).ToString('d\.hh\:mm\:ss'))

Processor: $($cpu.Name | Select-Object -First 1)
Cores: $($cpu.NumberOfCores | Measure-Object -Sum).Sum

Total RAM: $([math]::Round(($ram.Capacity | Measure-Object -Sum).Sum / 1GB, 2)) GB

Disk Drives:
"@
    
    foreach ($d in $disk) {
        $free = [math]::Round($d.FreeSpace / 1GB, 2)
        $total = [math]::Round($d.Size / 1GB, 2)
        $info += "`n  $($d.DeviceID): $free GB free of $total GB ($([math]::Round($free/$total*100, 1))%)"
    }
    
    return $info
}

function Update-ProcessList {
    $lstProcesses.Items.Clear()
    $procs = Get-Process | Sort-Object CPU -Descending | Select-Object -First 50
    foreach ($p in $procs) {
        $cpuUsage = if ($p.CPU) { [math]::Round($p.CPU, 2) } else { 0 }
        $lstProcesses.Items.Add("$($p.Id) | $($p.ProcessName) | CPU: $cpuUsage s | Mem: $([math]::Round($p.WorkingSet/1MB, 1)) MB")
    }
}

function Update-ServiceList {
    $lstServices.Items.Clear()
    $services = Get-Service | Sort-Object Status, Name
    foreach ($s in $services) {
        $statusIcon = switch ($s.Status) {
            "Running" { "▶" }
            "Stopped" { "■" }
            default { "●" }
        }
        $lstServices.Items.Add("$statusIcon $($s.Name) ($($s.DisplayName))")
    }
}

function Update-EventLog {
    $lstEvents.Items.Clear()
    $errors = Get-EventLog -LogName System -EntryType Error -Newest 20
    foreach ($e in $errors) {
        $lstEvents.Items.Add("[$($e.TimeGenerated)] $($e.Source): $($e.Message.Substring(0, [Math]::Min(100, $e.Message.Length)))...")
    }
}

# === Event Handlers ===

$btnRefreshInfo.Add_Click({
    $txtInfo.Text = Get-SystemInfo
})

$btnRefreshProc.Add_Click({
    Update-ProcessList
})

$btnKillProc.Add_Click({
    if ($lstProcesses.SelectedItem) {
        $procId = ($lstProcesses.SelectedItem -split ' \| ')[0]
        try {
            Stop-Process -Id $procId -Force -ErrorAction Stop
            [System.Windows.Forms.MessageBox]::Show("Process $procId ended successfully.", "Success", "OK", "Information")
            Update-ProcessList
        } catch {
            [System.Windows.Forms.MessageBox]::Show("Error ending process: $_", "Error", "OK", "Error")
        }
    } else {
        [System.Windows.Forms.MessageBox]::Show("Please select a process first.", "Selection Required", "OK", "Warning")
    }
})

$lstProcesses.Add_SelectedIndexChanged({
    if ($lstProcesses.SelectedItem) {
        $procId = ($lstProcesses.SelectedItem -split ' \| ')[0]
        try {
            $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
            if ($p) {
                $lblProcDetail.Text = "Path: $($p.Path)`nArgs: $($p.StartInfo.Arguments)"
            }
        } catch {}
    }
})

$btnRefreshServ.Add_Click({
    Update-ServiceList
})

$btnStartServ.Add_Click({
    if ($lstServices.SelectedItem) {
        $svcName = (($lstServices.SelectedItem -split ' \(')[0] -replace '^[▲■●] ', '')
        try {
            Start-Service -Name $svcName -ErrorAction Stop
            [System.Windows.Forms.MessageBox]::Show("Service $svcName started.", "Success", "OK", "Information")
            Update-ServiceList
        } catch {
            [System.Windows.Forms.MessageBox]::Show("Error starting service: $_", "Error", "OK", "Error")
        }
    }
})

$btnStopServ.Add_Click({
    if ($lstServices.SelectedItem) {
        $svcName = (($lstServices.SelectedItem -split ' \(')[0] -replace '^[▲■●] ', '')
        try {
            Stop-Service -Name $svcName -Force -ErrorAction Stop
            [System.Windows.Forms.MessageBox]::Show("Service $svcName stopped.", "Success", "OK", "Information")
            Update-ServiceList
        } catch {
            [System.Windows.Forms.MessageBox]::Show("Error stopping service: $_", "Error", "OK", "Error")
        }
    }
})

$btnRestartServ.Add_Click({
    if ($lstServices.SelectedItem) {
        $svcName = (($lstServices.SelectedItem -split ' \(')[0] -replace '^[▲■●] ', '')
        try {
            Restart-Service -Name $svcName -Force -ErrorAction Stop
            [System.Windows.Forms.MessageBox]::Show("Service $svcName restarted.", "Success", "OK", "Information")
            Update-ServiceList
        } catch {
            [System.Windows.Forms.MessageBox]::Show("Error restarting service: $_", "Error", "OK", "Error")
        }
    }
})

$btnRefreshEvt.Add_Click({
    Update-EventLog
})

# === Initialize ===
$txtInfo.Text = Get-SystemInfo
Update-ProcessList
Update-ServiceList
Update-EventLog

$form.Controls.Add($tabControl)
$form.ShowDialog()
