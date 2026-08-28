FROM python:3.11-slim

# ffmpeg is needed by faster-whisper/ctranslate2's audio decoding path.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY receive_and_transcribe.py db.py ./
COPY templates ./templates

EXPOSE 8787

# Unbuffered stdout so `docker compose logs -f` shows prints immediately.
ENV PYTHONUNBUFFERED=1

CMD ["python", "receive_and_transcribe.py"]
