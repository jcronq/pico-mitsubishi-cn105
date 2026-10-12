FROM python:3.12-slim

WORKDIR /app

# Copy project metadata first for layer caching.
COPY pyproject.toml ./
COPY mitsubishi_mcp ./mitsubishi_mcp

RUN pip install --no-cache-dir .

ENV PYTHONUNBUFFERED=1

# MCP stdio server — this image is meant to be run from a client that drives it
# over stdio (e.g. Claude Desktop launches the container as a subprocess).
# For a persistent broker-connected deployment, see docker-compose.example.yml.
ENTRYPOINT ["mitsubishi-mcp"]
