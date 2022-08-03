import { initialState } from "./store";

export const linksTrans = (state = initialState, action) => {
  switch (action.type) {
    case "addLinkTrans":
      var arr = Object.assign(state);
      return [...arr, action.payload];
    case "setLinksTrans":
      return action.payload;
    case "updateSelectedLinksTrans":
      var newState = Object.assign(state);
      newState[action.payload.index].selected = action.payload.value;
      return newState;
    default:
      return state;
  }
};

export const addLinkTrans = (link) => ({
  type: "addLinkTrans",
  payload: link,
});

export const setLinksTrans = (links) => ({
  type: "setLinksTrans",
  payload: links,
});

export const updateSelectedLinksTrans = (newValue) => ({
  type: "updateSelectedLinksTrans",
  payload: newValue,
});
