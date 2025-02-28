# Build the container image
podman build -t scan-viewer .

# Run the container (mount a folder with TIFF files)
podman run -it --rm \
    -e DISPLAY=$DISPLAY \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    -v ./tiff_files:/app/tiffs:ro \
    scan-viewer ./scan_viewer