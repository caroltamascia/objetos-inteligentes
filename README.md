## Como carregar o código no ESP32

1. Instale o **VS Code** e a extensão **PlatformIO**;
2. Entre na aba do **PlatformIO** e clique em **"Open Project"**, selecionando a pasta do projeto;
3. Aguarde o carregamento do projeto;
4. Conecte o **ESP32** ao computador;
5. Aperte `F1` e execute o comando:

## Como rodar a interface MQTT
Instale o **Mosquitto** com os seguintes comandos:

```bash
sudo apt install -y mosquitto mosquitto-clients
sudo systemctl enable mosquitto
sudo systemctl start mosquitto
```

## Por fim, inicie o aplicativo com os comandos:

```bash
cd pasta/do/projeto
cd mqtt/
python3 ./mqtt_realtime_plot.py
```
