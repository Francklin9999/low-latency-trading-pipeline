if [ "$(basename "$PWD")" == "scripts" ]; then
    cd ..
fi

echo "Starting engine..."
./build/bin/engine
