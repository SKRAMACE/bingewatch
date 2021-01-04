void
lime_set_gains(IO_HANDLE h, float lna, float tia, float pga)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    if (!chan) {
        return;
    }

    chan->_sdr.gain = lna;
    chan->tia_gain = tia;
    chan->pga_gain = pga;
}

void
lime_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth)
{
    soapy_set_rx(h, freq, rate, bandwidth);
}

IO_HANDLE
new_lime_rx_machine()
{
    return new_soapy_rx_machine("lime");
}

void
lime_set_log_level(char *level)
{
    soapy_set_log_level(level);
}
