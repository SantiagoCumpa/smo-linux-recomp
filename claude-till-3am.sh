#!/usr/bin/env bash
# Ejecuta un agente de Claude Code y, si se alcanza el límite de uso,
# reintenta periódicamente hasta que pueda continuar o llegue la hora tope.
#
# Uso:
#   ./claude-hasta-3am.sh "tu prompt / tarea para el agente"
#
# Variables opcionales:
#   DEADLINE="2026-11-02 03:00"   hora tope (zona TZ_LOCAL)
#   TZ_LOCAL="America/Lima"
#   RETRY_SECS=300                cada cuánto reintentar si hay límite (5 min)
#   CLAUDE_FLAGS="--dangerously-skip-permissions"   flags extra (opcional)

set -u

PROMPT="${1:?Falta el prompt. Uso: $0 \"tarea\"}"
TZ_LOCAL="${TZ_LOCAL:-America/Lima}"
DEADLINE="${DEADLINE:-2026-11-02 03:00}"
RETRY_SECS="${RETRY_SECS:-300}"
CLAUDE_FLAGS="${CLAUDE_FLAGS:-}"
LOG="${LOG:-$PWD/claude-agente.log}"

deadline_epoch=$(TZ="$TZ_LOCAL" date -d "$DEADLINE" +%s) || { echo "DEADLINE inválido"; exit 2; }

log() { echo "[$(TZ=$TZ_LOCAL date '+%F %T')] $*" | tee -a "$LOG"; }

# Patrones típicos del mensaje de límite de Claude Code
LIMIT_RE='usage limit|limit reached|rate limit|limit will reset|resets? (at|in)|too many requests|overloaded|429'

first=1
other_errors=0

log "Inicio. Tope: $DEADLINE ($TZ_LOCAL). Reintento cada ${RETRY_SECS}s."

while :; do
  now=$(date +%s)
  if (( now >= deadline_epoch )); then
    log "Se llegó a la hora tope ($DEADLINE). Detenido."
    exit 0
  fi

  if (( first )); then
    log "Ejecutando agente (primera corrida)..."
    out=$(claude -p "$PROMPT" $CLAUDE_FLAGS 2>&1); code=$?
  else
    log "Reanudando sesión anterior..."
    out=$(claude -p "Continúa exactamente donde quedaste con la tarea original: $PROMPT" \
          --continue $CLAUDE_FLAGS 2>&1); code=$?
  fi
  echo "$out" >> "$LOG"

  if grep -qiE "$LIMIT_RE" <<<"$out"; then
    first=0   # a partir de ahora, reanudar con --continue
    remaining=$(( deadline_epoch - $(date +%s) ))
    wait=$(( RETRY_SECS < remaining ? RETRY_SECS : remaining ))
    (( wait <= 0 )) && { log "Límite alcanzado y ya pasó la hora tope. Detenido."; exit 0; }
    log "Límite de uso alcanzado. Reintento en ${wait}s."
    sleep "$wait"
    continue
  fi

  if (( code == 0 )); then
    log "Agente terminó correctamente."
    exit 0
  fi

  # Error distinto al límite: pocos reintentos y salir
  other_errors=$(( other_errors + 1 ))
  first=0
  if (( other_errors >= 3 )); then
    log "3 errores no relacionados con el límite (código $code). Detenido."
    exit "$code"
  fi
  log "Error (código $code). Reintento ${other_errors}/3 en 60s."
  sleep 60
done
