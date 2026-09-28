#!/bin/bash
# Deploy cloud backend to VPS
# Usage: sh deploy/deploy.sh user@your-vps-ip

HOST="${1:-root@your-vps-ip}"
REMOTE_DIR="/opt/plant-alert-cloud"

echo "Deploying to $HOST:$REMOTE_DIR ..."

# Create remote directory
ssh "$HOST" "mkdir -p $REMOTE_DIR/uploads $REMOTE_DIR/data"

# Sync files
rsync -avz --exclude 'node_modules' --exclude 'uploads/*' --exclude 'data/alerts.db' \
    ./ "$HOST:$REMOTE_DIR/"

# Install deps and restart
ssh "$HOST" "cd $REMOTE_DIR && npm install --production && pm2 restart plant-alert 2>/dev/null || pm2 start server.js --name plant-alert"

echo "Done. Check with: curl http://$HOST:8080/api/health"
