@pushd %~dp0
pandoc --metadata title="Brightly" -s README.md -o docs/index.html
copy /y screenshot.png docs
@popd
