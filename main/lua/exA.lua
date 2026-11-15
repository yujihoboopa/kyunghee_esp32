slots = {}

for i = 1, 64 do
    slots[i] = {}
end

function sensor_tick(seq, deadline)
    for i = 1, 64 do
        slots[i] = {
            x = seq * 64 + i,
            y = seq * 64 + i * 2
        }
    end
end
