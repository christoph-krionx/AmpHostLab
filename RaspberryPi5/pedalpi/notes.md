sudo apt update
gets information about the latest version of packages available, doesn't download or install any package.

sudo apt upgrade
downloads and upgrades the packages to the new version.

mkdir -p ~/Downloads
cd ~/Downloads

wget "https://github.com/rerdavies/pipedal/releases/download/v2.0.102/pipedal_2.0.102_arm64.deb"
download package

sudo apt-get install ./pipedal_2.0.102_arm64.deb 
install package

pipedalconfig --enable
start pipedal services at boot time

pipedalconfig --restart