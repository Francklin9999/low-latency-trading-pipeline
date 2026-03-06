if [ "$(basename "$PWD")" == "scripts" ]; then
	cd ..
fi
rm -rf build
mkdir build
cd build
cmake ..
make all
