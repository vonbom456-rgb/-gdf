$ErrorActionPreference = "Stop"

function Pause-End {
    Write-Host ""
    Read-Host "Press Enter to close"
}

try {
    $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    Set-Location $scriptDir

    Write-Host "==========================================" -ForegroundColor Cyan
    Write-Host " TurretControl GitHub uploader FIXED v2 " -ForegroundColor Cyan
    Write-Host "==========================================" -ForegroundColor Cyan
    Write-Host ""

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        throw "Git is not installed. Run the previous installer script once first."
    }

    git --version

    Write-Host "[1/7] Initializing repository..." -ForegroundColor Cyan
    if (-not (Test-Path ".git")) {
        git init
        if ($LASTEXITCODE -ne 0) { throw "git init failed." }
    }

    Write-Host "[2/7] Setting branch main..." -ForegroundColor Cyan
    git branch -M main
    if ($LASTEXITCODE -ne 0) { throw "Could not set branch main." }

    Write-Host "[3/7] Setting GitHub remote..." -ForegroundColor Cyan
    $remotes = @(git remote)
    if ($remotes -contains "origin") {
        git remote set-url origin "https://github.com/vonbom456-rgb/-gdf.git"
    } else {
        git remote add origin "https://github.com/vonbom456-rgb/-gdf.git"
    }
    if ($LASTEXITCODE -ne 0) { throw "Could not configure remote origin." }

    Write-Host "[4/7] Adding files..." -ForegroundColor Cyan
    git add .
    if ($LASTEXITCODE -ne 0) { throw "git add failed." }

    Write-Host "[5/7] Creating commit if needed..." -ForegroundColor Cyan
    git diff --cached --quiet
    if ($LASTEXITCODE -ne 0) {
        git commit -m "Update TurretControl"
        if ($LASTEXITCODE -ne 0) { throw "git commit failed." }
    } else {
        Write-Host "No new changes to commit." -ForegroundColor DarkGray
    }

    Write-Host "[6/7] Syncing remote if main already exists..." -ForegroundColor Cyan
    cmd /c "git ls-remote --exit-code --heads origin main >nul 2>nul"
    if ($LASTEXITCODE -eq 0) {
        git pull --rebase origin main
        if ($LASTEXITCODE -ne 0) {
            throw "git pull --rebase failed. Send me a screenshot."
        }
    } else {
        Write-Host "Remote main does not exist yet. First push will create it." -ForegroundColor DarkGray
    }

    Write-Host "[7/7] Pushing to GitHub..." -ForegroundColor Cyan
    git push -u origin main
    if ($LASTEXITCODE -ne 0) {
        throw "git push failed. If GitHub asks you to sign in, sign in and run this script again."
    }

    Write-Host ""
    Write-Host "SUCCESS!" -ForegroundColor Green
    Write-Host "Project uploaded to:"
    Write-Host "https://github.com/vonbom456-rgb/-gdf" -ForegroundColor Green
    Write-Host ""
    Write-Host "Opening GitHub Actions..." -ForegroundColor Green
    Start-Process "https://github.com/vonbom456-rgb/-gdf/actions"

    Pause-End
}
catch {
    Write-Host ""
    Write-Host "ERROR:" -ForegroundColor Red
    Write-Host $_.Exception.Message -ForegroundColor Red
    Write-Host ""
    Write-Host "Send me a screenshot of this window." -ForegroundColor Yellow
    Pause-End
    exit 1
}
