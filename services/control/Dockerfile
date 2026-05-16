FROM node:22-bookworm-slim

WORKDIR /app

COPY services/control/package.json ./package.json
RUN npm install --omit=dev

COPY services/control/server.js ./server.js
COPY dashboard /app/dashboard

EXPOSE 8080

CMD ["npm", "start"]
