FROM gcc:13
RUN apt-get update && apt-get install -y curl && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN g++ -std=c++17 -O2 server.cpp -o med_server -lpthread
EXPOSE 8080
CMD ["./med_server"]

