$ErrorActionPreference = "Stop"

function Pause-End {
    Write-Host ""
    Read-Host "Press Enter to close"
}

try {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    Set-Location $scriptDir

    Write-Host "==========================================" -ForegroundColor Cyan
    Write-Host " TurretControl automatic GitHub uploader " -ForegroundColor Cyan
    Write-Host "==========================================" -ForegroundColor Cyan
    Write-Host ""

    # Ensure admin because winget/Git installation may need elevation.
    $isAdmin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()
        ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

    if (-not $isAdmin) {
        Write-Host "[INFO] Restarting as Administrator..." -ForegroundColor Yellow
        Start-Process powershell.exe -Verb RunAs -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", "`"$($MyInvocation.MyCommand.Path)`""
        )
        exit
    }

    # Install Git if missing.
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        Write-Host "[1/8] Git not found. Installing Git for Windows via winget..." -ForegroundColor Yellow

        $winget = Get-Command winget -ErrorAction SilentlyContinue
        if (-not $winget) {
            throw "winget was not found. Install App Installer from Microsoft Store, then run this script again."
        }

        winget install --id Git.Git -e --source winget `
            --accept-package-agreements `
            --accept-source-agreements `
            --silent

        if ($LASTEXITCODE -ne 0) {
            throw "Git installation failed. winget exit code: $LASTEXITCODE"
        }

        # Refresh PATH in current process.
        $machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
        $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
        $env:Path = "$machinePath;$userPath"

        # Fallback common Git path.
        if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
            $gitCmd = "C:\Program Files\Git\cmd"
            if (Test-Path $gitCmd) {
                $env:Path = "$gitCmd;$env:Path"
            }
        }
    } else {
        Write-Host "[1/8] Git is already installed." -ForegroundColor Green
    }

    git --version
    if ($LASTEXITCODE -ne 0) {
        throw "Git is installed but cannot be executed."
    }

    # Configure identity only if missing.
    $name = git config --global user.name 2>$null
    $email = git config --global user.email 2>$null

    if ([string]::IsNullOrWhiteSpace($name)) {
        git config --global user.name "vonbom456-rgb"
    }

    if ([string]::IsNullOrWhiteSpace($email)) {
        $enteredEmail = Read-Host "Enter the email used for your Git commits"
        if ([string]::IsNullOrWhiteSpace($enteredEmail)) {
            throw "Git email is required to create commits."
        }
        git config --global user.email $enteredEmail
    }

    Write-Host "[2/8] Initializing repository..." -ForegroundColor Cyan
    if (-not (Test-Path ".git")) {
        git init
        if ($LASTEXITCODE -ne 0) { throw "git init failed." }
    }

    Write-Host "[3/8] Setting branch main..." -ForegroundColor Cyan
    git branch -M main
    if ($LASTEXITCODE -ne 0) { throw "Could not set branch main." }

    Write-Host "[4/8] Setting GitHub remote..." -ForegroundColor Cyan
    $origin = git remote get-url origin 2>$null
    if ($LASTEXITCODE -eq 0 -and $origin) {
        git remote set-url origin "https://github.com/vonbom456-rgb/-gdf.git"
    } else {
        git remote add origin "https://github.com/vonbom456-rgb/-gdf.git"
    }
    if ($LASTEXITCODE -ne 0) { throw "Could not configure remote origin." }

    Write-Host "[5/8] Adding project files..." -ForegroundColor Cyan
    git add .
    if ($LASTEXITCODE -ne 0) { throw "git add failed." }

    Write-Host "[6/8] Creating commit if needed..." -ForegroundColor Cyan
    git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) {
        git commit -m "Update TurretControl"
        if ($LASTEXITCODE -ne 0) { throw "git commit failed." }
    } else {
        Write-Host "No new changes to commit." -ForegroundColor DarkGray
    }

    Write-Host "[7/8] Syncing existing GitHub branch if present..." -ForegroundColor Cyan
    git ls-remote --exit-code --heads origin main *> $null
    if ($LASTEXITCODE -eq 0) {
        git pull --rebase origin main
        if ($LASTEXITCODE -ne 0) {
            throw "git pull --rebase failed. The remote repository may contain conflicting files."
        }
    }

    Write-Host "[8/8] Uploading to GitHub..." -ForegroundColor Cyan
    git push -u origin main

    if ($LASTEXITCODE -ne 0) {
        throw "git push failed. If GitHub opened a login window, sign in and run this script again."
    }

    Write-Host ""
    Write-Host "SUCCESS: Project uploaded." -ForegroundColor Green
    Write-Host "Repository: https://github.com/vonbom456-rgb/-gdf"
    Write-Host "Opening GitHub Actions..." -ForegroundColor Green

    Start-Process "https://github.com/vonbom456-rgb/-gdf/actions"

    Pause-End
}
catch {
    Write-Host ""
    Write-Host "ERROR:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host ""
    Write-Host "Take a screenshot of this window and send it to me." -ForegroundColor Yellow
    Pause-End
    exit 1
}
