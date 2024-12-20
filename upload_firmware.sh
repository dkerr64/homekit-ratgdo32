#!/usr/bin/env sh
IP=${1}
FILE=${2}
if [ $(which md5sum) ]; then
    # Linux
    MD5=$(md5sum "${FILE}" | cut -d' ' -f1)
    SIZE=$(stat -c%s "${FILE}")
    elif [ $(which md5) ]; then
    # macOS
    MD5=$(md5 -q "${FILE}")
    SIZE=$(stat -f%z "${FILE}")
fi
if [ -z "${MD5}" ]; then
    echo "Unable to calculate MD5 hash for file ${$FILE}, terminating"
    exit 1
fi
echo "Calculated MD5 hash for file ${FILE}: ${MD5}, Size: ${SIZE} bytes"
JSON="{\"md5\":\"${MD5}\",\"size\":${SIZE},\"uuid\":\"n/a\"}"
echo "Uploading file..."
RESPONSE=$(curl -s -w ">>>>>%{http_code}" -F "content=@${FILE}" "http://${IP}/update?action=update&size=${SIZE}&md5=${MD5}")
BODY=$(echo ${RESPONSE} | awk -F'>>>>>' '{print $1}')
RC=$(echo ${RESPONSE} | awk -F'>>>>>' '{print $2}')
echo ${BODY}
if [ "${RC}" = "200" ]; then
    echo "Success, reboot RATGDO, please allow 30 seconds to complete..."
    curl -s -X POST "http://${IP}/reboot"
    exit 0
fi
echo "Upload failure, RATGDO device NOT rebooted..."
echo "To reboot device issue command: curl -s -X POST http://${IP}/reboot"
exit 1
