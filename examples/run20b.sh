echo "!!!please use generate_hostfile.sh to set hostfile for enough nodes before training"
export WORLD_SIZE=${WORLD_SIZE:-48}
export MICRO_BATCH=${MICRO_BATCH:-1}
export NLAYERS=${NLAYERS:-44}
export HIDDEN=${HIDDEN:-6144}
export HEADS=${HEADS:-64}
export SEQ=${SEQ:-2048}
export TRAIN_ITER=${TRAIN_ITER:-20}
export ZERO_STAGE=${ZERO_STAGE:-3}
export TP=${TP:-1}
export PP=${PP:-1}
export GLOBAL_BATCH=$(( $WORLD_SIZE * $MICRO_BATCH / $TP / $PP ))

export DS_CONFIG=${PWD}/"ds_stage${ZERO_STAGE}_mb${MICRO_BATCH}_gb${GLOBAL_BATCH}_pp${PP}_tp${TP}_bf16.json"

bash $LLM_DK_DIR/intel-extension-for-deepspeed/examples/generate_config.sh
bash $LLM_DK_DIR/intel-extension-for-deepspeed/examples/gpt.sh