SSAM_RPC_DIR=/usr/bin
HW_DPU_RPC_SCRIPT=$SSAM_RPC_DIR/hw_dpu_rpc.py
UPGRADE_SCRIPT=$SSAM_RPC_DIR/upgrade_rpc.py
RPC_SCRIPT=$SSAM_RPC_DIR/rpc.py
RUN_DIR=/var/run
SAVE_CONF=/root/save.conf
CPU_MASK=0x7ff80
SSAM_HOT_UPGRADE_TIME_OUT=300
SSAM_LOG=/var/log/dpak/ssam/ssam.log
PID_FILE=$RUN_DIR/ssam.pid
PID_UPGRADE_FILE=$RUN_DIR/ssam_upgrade.pid
SOCKET_FILE=$RUN_DIR/ssam_sock.conf
SOCKET_UPGRADE_FILE=$RUN_DIR/ssam_upgrade_sock.conf

function log_info
{
    echo $1
    logger -p user.info -i -t ssam_upgrade $1
}

function log_notice
{
    echo $1
    logger -p user.notice -i -t ssam_upgrade $1
}

function log_err
{
    echo $1
    logger -p user.err -i -t ssam_upgrade $1
}

function clean_env {
    rm -rf $PID_FILE
    rm -rf $PID_UPGRADE_FILE
    rm -rf $SOCKET_FILE
    rm -rf $SOCKET_UPGRADE_FILE
}

function check_process_exist {
    local new_socket=$1
    local ssam_count=$(pidof ssam | wc -w 2>/dev/null)
    if [ $ssam_count -ne 1 ]; then
        log_info "there are $ssam_count ssam process, please check"
        return 1
    fi

    local ssam_pid=$(pidof ssam 2>/dev/null)
    if [ -z $ssam_pid ]; then
        log_err "ssam process do not exist, no need to upgrade"
        return 2
    fi

    echo $ssam_pid > $PID_FILE
    local sock=$(ss -ap |grep "pid=$ssam_pid" |grep "sock" |awk '{print $5}' 2>/dev/null)
    if [[ "$sock" == "$new_socket" ]]; then
        log_err "the input rpc scoket has been used, please use another socket. current input socket :$new_socket"
        return 3
    fi

    echo $sock > $SOCKET_FILE
    log_notice "old ssam exist, pid: $ssam_pid , rpc socket: $sock"
    return 0
}

function check_spdk_process {
    local new_socket=$1
    log_notice "start check old ssam status ..."
    check_process_exist $new_socket
    ret=$?
    if [ $ret -ne 0 ]; then 
        exit 2
    fi
    return 0
}

function check_ssam_hot_upgrade_process_exist {
    local sock=$1
    local old_ssam_pid=$(cat $PID_FILE)
   
    time_limit=$SSAM_HOT_UPGRADE_TIME_OUT 
    while [ $time_limit -gt 0 ];
    do
        local ssam_pid=($(pidof ssam 2>/dev/null))
        for pid in "${ssam_pid[@]}"; do
            if [ $pid -ne  $old_ssam_pid ]; then
                echo $pid > $PID_UPGRADE_FILE
                echo $sock > $SOCKET_UPGRADE_FILE
                log_notice "new ssam process started , pid :$pid, rpc socket: $sock"
                return 0
            fi
        done
        time_limit=$(($time_limit-1))
        sleep 1
    done
    log_err "time out $SSAM_HOT_UPGRADE_TIME_OUT s, ssam hot upgrade process does not exist!"
    return 1
}

function start_hot_upgrade_process {
    local daemon=$1
    local socket=$2
    log_notice "start load new ssam process ..."
    setsid nohup $daemon -c $SAVE_CONF -r $socket -m $CPU_MASK --hot-upgrade --disable-cpumask-locks >> $SSAM_LOG 2>&1 &
    sleep 5
    check_ssam_hot_upgrade_process_exist $socket
    if [ $? -eq 1 ]; then 
        log_err "hot upgrade process check failed"
        clean_env
        exit 2
    fi
}

function check_hot_upgrade_process_init_done {
    local i=0
    local sock=$1
    log_notice "start to check new process status..."
    while [ $i -lt $SSAM_HOT_UPGRADE_TIME_OUT ];
    do 
        $RPC_SCRIPT -s $sock spdk_get_version >/dev/null 2>&1
        if [ $? -eq 0 ]; then
            sleep 1
            local status=$($HW_DPU_RPC_SCRIPT -s $sock check_hot_upgrade_status 2>/dev/null)
            if [ $? -eq 0 ]  && [[ "$status" == "true" ]]; then
                log_info "hot upgrade process init success"
                return 0;
            fi
        fi   
        i=$(($i + 1))
        sleep 1
    done
    log_err "new ssam init time out, times :$i"
    return 1
}

function kill_ssam {
    local pid=$1

    if [ -z "$pid" ] || [ ! -d "/proc/$pid" ]; then 
        log_info "kill ssam failed ,process maybe not exist. pid :$pid"
        return 1
    fi

    kill -9 $pid
    log_info "kill ssam process success. pid :$pid"
    return 0
}

function stop_ssam_process {
    local new_socket=$1
    local new_pid=$(cat $PID_UPGRADE_FILE)
    check_hot_upgrade_process_init_done $new_socket 
    if [ $? -ne 0 ]; then
        log_notice "hot upgrade new ssam init failed"
        kill_ssam $new_pid
        clean_env
        exit 3
    fi

    local old_ssam_pid=$(cat $PID_FILE)
    local old_ssam_sock=$(cat $SOCKET_FILE)
    sleep 0.5
    kill -STOP $new_pid >/dev/null 2>&1
    kill_ssam $old_ssam_pid
}

function start_hot_upgrade_io_poll {
    local new_socket=$1
    sleep 0.05
    local new_pid=$(cat $PID_UPGRADE_FILE)
    kill -CONT $new_pid >/dev/null 2>&1
    ret=$($UPGRADE_SCRIPT -s $new_socket hot_upgrade_start_io_poll 2>/dev/null)
        if [[ $? -eq 0 ]] && [[ "$ret" == "true" ]]; then
            log_notice "ssam hot upgrade success"
            return 0
        fi
    # clean new ssam to rollback 
   
    kill_ssam $new_pid
    log_err "start io polling failed, ret:$ret"
    log_notice "old ssam process has been killed ,please use rollback to recover"
    clean_env
    exit 4
}

function do_upgrade {
    local new_process=$1
    local socket_path=$2 
    if [ ! -f "$new_process" ] || [ ! -x "$new_process" ]; then
        log_err "The binary file does not exist or is not executable"
        exit 1
    fi

    log_notice "start hot upgrade ..."
    clean_env
    check_spdk_process $socket_path
    start_hot_upgrade_process $new_process $socket_path
    stop_ssam_process $socket_path
    start_hot_upgrade_io_poll $socket_path
}

function check_ssam_process_exist {
   
    time_limit=$SSAM_HOT_UPGRADE_TIME_OUT 
    while [ $time_limit -gt 0 ];
    do
        local ssam_count=($(pidof ssam |wc -w 2>/dev/null))
        if [ $ssam_count -gt 0 ]; then
            local pid=($(pidof ssam 2>/dev/null))
            log_notice "ssam process exitst , pid :$pid" 
            return 0;
        fi

        time_limit=$(($time_limit-1))
        sleep 1
    done

    log_err "time out 30s, ssam process does not exist!"
    return 1
}

function hot_restart_process {
    local daemon=$1
    local socket=$2
    log_notice "start ssam process hot restart"
    setsid nohup $daemon -c $SAVE_CONF -r $socket -m $CPU_MASK --hot-restart --disable-cpumask-locks >> $SSAM_LOG 2>&1 &
    sleep 5
    check_ssam_process_exist
    if [ $? -ne 0 ]; then 
        log_err "rollback ssam failed"
        exit 2
    fi
    log_notice "rollback ssam complete"
}

function kill_ssam_process {
    log_notice "start clean up existing ssam processes"
    local ssam_pid=($(pidof ssam 2>/dev/null))
    for pid in "${ssam_pid[@]}"; do
        kill_ssam $pid
        log_info "kill ssam process. pid : $pid"
    done
}

function rollback_ssam {
    local old_process=$1
    local socket_path=$2 
    if [ ! -f "$old_process" ] || [ ! -x "$old_process" ]; then
        log_err "The binary file does not exist or is not executable."
        exit 1
    fi

    log_notice "start rollback ..."
    kill_ssam_process
    sleep 1
    hot_restart_process $old_process $socket_path
}

main() {
    local operation=$1
    case "$operation" in 
        upgrade)
            if [ $# -ne 3 ]; then
                echo "Usage: $0 upgrade <path of ssam to upgrade> <path of socket>"
                exit 1
            fi
            local new_process_path=$2
            local new_socket_path=$3
            do_upgrade "$new_process_path" "$new_socket_path"
        ;;
        rollback)
            if [ $# -ne 3 ]; then
                echo "Usage: $0 rollback <path of ssam to rollback> <path of socket>"
                exit 1
            fi
            local old_process_path=$2
            local old_socket_path=$3
            rollback_ssam "$old_process_path" "$old_socket_path"
        ;;
        *)
            echo "Help:"
            echo " hot_upgrade.sh <upgrade | rollback> [path of ssam] [path of socket]"
            echo " upgrade [path of ssam] [path of scoket]   hot upgrade ssam process"
            echo " rollback [path of ssam] [path of scoket]  rollback ssam process"
            exit 1
        ;;
        esac
}

main "$@"