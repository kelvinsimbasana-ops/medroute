FROM gcc:13
WORKDIR /app
COPY . .
RUN g++ -std=c++17 -O2 server.cpp -o med_server -lpthread
EXPOSE 8080
CMD ["./med_server"]
