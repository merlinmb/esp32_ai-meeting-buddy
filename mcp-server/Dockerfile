FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY server.py server_http.py ./

EXPOSE 8788

ENV PYTHONUNBUFFERED=1

CMD ["python3", "server_http.py"]
