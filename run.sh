# Build the container image
podman build -t scan-viewer .

# Run the container (make sure you have X11 forwarding set up)
podman run -it --rm \
    -e DISPLAY=$DISPLAY \
    -v /tmp/.X11-unix:/tmp/.X11-unix \
    scan-viewer ./scan_viewer