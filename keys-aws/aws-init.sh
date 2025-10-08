#!/bin/zsh

# --- CONFIGURACIÓN ---
KEY_FILE="esp32-key.pem"
USER="ubuntu"
HOST="ec2-18-191-73-45.us-east-2.compute.amazonaws.com"
# ---------------------

# 1. Obtener la ruta del script y cambiar a ese directorio
# Esto asegura que el script encuentre la clave sin importar desde dónde se ejecute.
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )"
cd "$SCRIPT_DIR"

echo "✅ Directorio actual: $PWD"

# 2. Verificar si el archivo de la clave existe
if [ ! -f "$KEY_FILE" ]; then
    echo "❌ Error: No se encontró el archivo de clave: $KEY_FILE"
    exit 1
fi

# 3. Ajustar los permisos de la clave SSH
# Los permisos 400 (sólo lectura para el propietario) son los requeridos por SSH.
echo "🔑 Ajustando permisos de $KEY_FILE a 400..."
chmod 400 "$KEY_FILE"

# 4. Conectarse al servidor usando SSH
echo "🔗 Conectando a $USER@$HOST con la clave $KEY_FILE..."
ssh -i "$KEY_FILE" "$USER"@"$HOST"

# 5. Mensaje de finalización
if [ $? -eq 0 ]; then
    echo "🎉 Conexión SSH terminada."
else
    echo "🛑 La conexión SSH falló o se interrumpió."
fi
