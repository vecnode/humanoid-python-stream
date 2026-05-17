from dataclasses import dataclass
from queue import Queue, Empty, Full
from typing import Any, Dict, Optional


@dataclass
class ConditioningItem:
    pose: Any
    phase: Any
    meta: Dict[str, Any]


class ConditioningQueue:
    def __init__(self, maxsize: int = 128) -> None:
        self._queue: Queue = Queue(maxsize=maxsize)

    def put_nowait(self, item: ConditioningItem) -> bool:
        try:
            self._queue.put_nowait(item)
            return True
        except Full:
            return False

    def get_nowait(self) -> Optional[ConditioningItem]:
        try:
            return self._queue.get_nowait()
        except Empty:
            return None
